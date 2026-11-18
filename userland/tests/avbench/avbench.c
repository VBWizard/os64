// avbench — how fast libav.so decodes video on this machine, and converts
// it to a window's pixels, and whether it decodes the right pictures.
//
//   avbench [-c | -s] [-n frames] [-k crc] [-x WxH|natural [-e] [-K crc]] file
//   avbench all <directory> [runs] [-x WxH|natural]
//
// The file is read whole into memory first, so the disk is not in the
// figure, and FFmpeg gets its bytes through the read and seek callbacks, as
// every caller does. The first video stream is decoded on the calling
// thread (avbench starts none, so `env PINCPU=<n>` pins all of it), sound
// ignored, and each packet's send-and-receive is timed with micros(): only
// FFmpeg's calls, never the CRC below, which runs between the clocks.
//
//   (neither)  FFmpeg's own choice of routines for this CPU
//   -s         the SSE tiers: AVX, AVX2 and FMA3 masked out of what FFmpeg
//              detected (the host measurement's tier 2)
//   -c         the C routines: av_force_cpu_flags(0). Not the same as a
//              library built without assembly: the inline assembly FFmpeg
//              chooses at compile time stays in, and the output says so
//   -n frames  stop after that many frames
//   -k crc     the CRC the frames must give (hex); a different one exits 4
//   -x WxH     also convert every frame to XRGB8888 (BGRA in memory, the
//              window canvas's format) at W x H, the way a player will:
//              sws_scale_frame on a dynamic context (never the legacy
//              API), threads 1, into a destination avbench owns (64-byte
//              aligned, more than FFmpeg's routines ask of a buffer, and
//              rows W * 4 bytes apart, as a canvas's are), from a
//              shallow copy of the decoded frame with its colours filled
//              in (below). Each frame is converted twice, bilinear and
//              fast bilinear, each by its own scaler and timed apart.
//              `natural` is the picture's own size: conversion, no scaling
//   -e         with -x: the bilinear conversion in FFmpeg's bit-exact mode,
//              whose pixels do not depend on which routines FFmpeg chose,
//              and a CRC-32 over them (every byte of every row)
//   -K crc     with -e: the CRC the converted pixels must give; a different
//              one exits 4
//
// It prints the picture size and rate, the CPU flags FFmpeg reports it is
// using, frames decoded, frames per second and that as a multiple of the
// clip's own rate, the worst frame and the 99th percentile in milliseconds,
// and a CRC-32 (zlib's) over every decoded frame's planes, visible rows and
// columns only. H.264 decoding is bit-exact, so the CRC is the same for
// every mode, on every machine, and on the host (the host's tool computes
// exactly this). With -x, a second line gives, average and worst, a
// frame's conversion with each filter, and per packet that gave frames
// (the flush's included) its decode alone and its decode with the
// conversions of what it gave, so `decode+bilinear` is `decode` plus that
// packet's `bilinear`. With -e the bilinear figures are the bit-exact
// scaler's, labelled so, and slower than a player's. Each scaler's first
// conversion builds its graph, once, and is made untimed before the first
// frame is timed.
//
// THE COLOURS: the matrix and range the stream states; with no matrix,
// BT.709 for a picture 1280 or more wide or taller than 576 and BT.601
// below (mpv's rule: standard definition was 601's); with no range,
// limited. The destination takes the source's primaries and transfer, so
// the scaler converts neither (left unspecified, it would map a wider
// gamut, BT.2020, PQ or HLG, to BT.709).
//
// `all` runs every .mp4 in the directory in each of the three modes, `runs`
// times each (3 if absent), and appends one line a run to /home/avbench.txt,
// which is the file to send back; with -x, each line carries the
// conversion's figures too.
//
// Exit: 0 decoded to the end, 1 usage, 2 cannot read or open the file,
// 3 reading, decoding or converting failed partway, or the conversion
// could not be set up (its memory, its scalers), 4 a CRC differs from
// -k or -K. With -k or -K, a match exits AVBENCH_CRC_OK instead of 0:
// testrun's badge, a word spelled in hex, so a program that ended for any
// other reason cannot pass for this one.

#include "os64/os64.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/cpu.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libswscale/swscale.h"

#define RESULTS "/home/avbench.txt"
#define AVBENCH_CRC_OK 0xAB0C0000   // "AB OK": -k matched

enum { MODE_AUTO, MODE_SSE, MODE_C };
static const char *const k_mode_name[] = { "auto", "sse", "c" };

// ------------------------------------------------------------ the CRC

static uint32_t s_crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--)
        crc = s_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

// Every plane of one frame, the visible bytes of each row only: the chroma
// planes are as tall as the format's shift makes them, never the padding.
static uint32_t crc_frame(uint32_t crc, const AVFrame *f)
{
    enum AVPixelFormat fmt = (enum AVPixelFormat)f->format;
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
    int planes = av_pix_fmt_count_planes(fmt);
    for (int p = 0; p < planes; p++) {
        int bytes = av_image_get_linesize(fmt, f->width, p);
        int rows = (p == 1 || p == 2) ? -((-f->height) >> d->log2_chroma_h) : f->height;
        for (int y = 0; y < rows; y++)
            crc = crc_update(crc, f->data[p] + (ptrdiff_t)y * f->linesize[p], (size_t)bytes);
    }
    return crc;
}

// ------------------------------------------------------------ memory input

typedef struct {
    const uint8_t *data;
    int64_t size, at;
} Memory;

static int memory_read(void *opaque, uint8_t *out, int cap)
{
    Memory *m = opaque;
    int64_t left = m->size - m->at;
    if (left <= 0) return AVERROR_EOF;
    int n = cap < left ? cap : (int)left;
    os64_memcpy(out, m->data + m->at, (size_t)n);
    m->at += n;
    return n;
}

static int64_t memory_seek(void *opaque, int64_t offset, int whence)
{
    Memory *m = opaque;
    if (whence == AVSEEK_SIZE) return m->size;
    whence &= ~AVSEEK_FORCE;
    int64_t to = whence == SEEK_SET ? offset : whence == SEEK_CUR ? m->at + offset
               : whence == SEEK_END ? m->size + offset : -1;
    if (to < 0 || to > m->size) return AVERROR(EINVAL);
    m->at = to;
    return to;
}

static uint8_t *read_whole(const char *path, int64_t *size)
{
    os64_dirent_t e;
    if (os64_stat(path, &e) < 0 || e.size == 0) return NULL;
    uint8_t *buf = os64_malloc(e.size);
    if (!buf) return NULL;
    int64_t h = os64_open(path, "r");
    if (h < 0) { os64_free(buf); return NULL; }
    uint64_t got = 0;
    while (got < e.size) {
        int64_t n = os64_read((int32_t)h, buf + got, e.size - got);
        if (n <= 0) break;
        got += (uint64_t)n;
    }
    os64_close((int32_t)h);
    if (got != e.size) { os64_free(buf); return NULL; }
    *size = (int64_t)e.size;
    return buf;
}

// ------------------------------------------------------------ the run

// FFmpeg's words reach the terminal only when they are errors.
static void on_log(void *avcl, int level, const char *fmt, va_list ap)
{
    (void)avcl;
    if (level > AV_LOG_ERROR) return;
    char line[256];
    os64_vsnprintf(line, sizeof(line), fmt, ap);
    os64_printf("avbench: ffmpeg: %s", line);
}

static void flag_names(char *out, size_t cap, unsigned flags)
{
    static const struct { unsigned bit; const char *name; } k[] = {
        { AV_CPU_FLAG_MMX, "mmx" }, { AV_CPU_FLAG_MMXEXT, "mmxext" }, { AV_CPU_FLAG_SSE, "sse" },
        { AV_CPU_FLAG_SSE2, "sse2" }, { AV_CPU_FLAG_SSE3, "sse3" }, { AV_CPU_FLAG_SSSE3, "ssse3" },
        { AV_CPU_FLAG_SSE4, "sse4.1" }, { AV_CPU_FLAG_SSE42, "sse4.2" }, { AV_CPU_FLAG_AVX, "avx" },
        { AV_CPU_FLAG_AVXSLOW, "avxslow" }, { AV_CPU_FLAG_AVX2, "avx2" }, { AV_CPU_FLAG_FMA3, "fma3" },
        { AV_CPU_FLAG_BMI1, "bmi1" }, { AV_CPU_FLAG_BMI2, "bmi2" }, { AV_CPU_FLAG_AVX512, "avx512" },
        { AV_CPU_FLAG_SLOW_GATHER, "slowgather" },
    };
    size_t at = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (flags & k[i].bit)
            at += (size_t)os64_snprintf(out + at, at < cap ? cap - at : 0, "%s%s", at ? " " : "", k[i].name);
    if (!at) os64_snprintf(out, cap, "none (C)");
}

static void sort_us(int64_t *a, size_t n)
{
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++) {
            int64_t v = a[i];
            size_t j = i;
            for (; j >= gap && a[j - gap] > v; j -= gap) a[j] = a[j - gap];
            a[j] = v;
        }
}

// ------------------------------------------------------------ the conversion

// Times, one a frame or one a packet, grown as they come.
typedef struct {
    int64_t *v;
    size_t n, cap;
} Series;

static int series_push(Series *s, int64_t us)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 1024;
        int64_t *bigger = os64_realloc(s->v, cap * sizeof(int64_t));
        if (!bigger) return -1;
        s->v = bigger;
        s->cap = cap;
    }
    s->v[s->n++] = us;
    return 0;
}

typedef struct {
    int64_t avg_us, worst_us;
} Cost;

static Cost series_cost(const Series *s)
{
    Cost c = { 0, 0 };
    int64_t sum = 0;
    for (size_t i = 0; i < s->n; i++) {
        sum += s->v[i];
        if (s->v[i] > c.worst_us) c.worst_us = s->v[i];
    }
    c.avg_us = s->n ? sum / (int64_t)s->n : 0;
    return c;
}

// What -x, -e and -K asked for.
typedef struct {
    bool on;
    bool natural;      // the picture's own size
    int width, height;
    bool exact;
    bool have_crc;
    uint32_t want_crc;
} Scale;

// The colours a decoded frame is converted from, written into the frame
// itself (avbench's own after avcodec_receive_frame): THE COLOURS in the
// header.
static void colour_rule(AVFrame *f)
{
    if (f->colorspace == AVCOL_SPC_UNSPECIFIED)
        f->colorspace = (f->width >= 1280 || f->height > 576) ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    if (f->color_range == AVCOL_RANGE_UNSPECIFIED)
        f->color_range = AVCOL_RANGE_MPEG;
}

// A dynamic context (sws_scale_frame builds its graph from the frames it is
// given), one thread.
static SwsContext *scaler_new(unsigned flags)
{
    SwsContext *c = sws_alloc_context();
    if (c) {
        c->flags = flags;
        c->threads = 1;
    }
    return c;
}

// The destination: memory avbench owns, in an AVFrame from av_frame_alloc
// (AVFrame's size is not FFmpeg's ABI, so a frame FFmpeg reads is always
// one it allocated) that holds no buffer of FFmpeg's. NULL when memory ran
// out. Its primaries and transfer are set to each source's before each
// conversion (convert).
static AVFrame *canvas_frame(uint8_t *pixels, int width, int height)
{
    AVFrame *f = av_frame_alloc();
    if (f == NULL) return NULL;
    f->width = width;
    f->height = height;
    f->format = AV_PIX_FMT_BGRA;
    f->data[0] = pixels;
    f->linesize[0] = width * 4;
    f->colorspace = AVCOL_SPC_RGB;
    f->color_range = AVCOL_RANGE_JPEG;
    return f;
}

typedef struct {
    int width, height;
    double clip_fps, fps;
    long frames;
    int64_t worst_us, p99_us;
    uint32_t crc;
    char flags[160];
    // -x
    int scale_width, scale_height;
    bool scale_exact;        // the bilinear figures are the bit-exact scaler's
    Cost decode, bilinear, fast, with_bilinear, with_fast;
    uint32_t scale_crc;      // -e
} Result;

// Both conversions of one decoded frame, timed apart; the bit-exact one's
// CRC folded in between the clocks. 0, or -1 when the scaler refused.
static int convert(SwsContext *bilinear, SwsContext *fast, AVFrame *canvas, AVFrame *decoded, bool crc_it,
                   uint32_t *crc, int64_t *bilinear_us, int64_t *fast_us)
{
    colour_rule(decoded);
    // Primaries and transfer the same on both sides: the scaler converts
    // neither (THE COLOURS).
    canvas->color_primaries = decoded->color_primaries;
    canvas->color_trc = decoded->color_trc;
    // An interlaced frame (H.264 says so per frame) is converted field by
    // field; the scaler refuses a source and destination that disagree
    // (libswscale/swscale.c, sws_frame_setup), and it does not deinterlace.
    canvas->flags = (canvas->flags & ~AV_FRAME_FLAG_INTERLACED) | (decoded->flags & AV_FRAME_FLAG_INTERLACED);
    int64_t t0 = os64_micros();
    int r = sws_scale_frame(bilinear, canvas, decoded);
    *bilinear_us = os64_micros() - t0;
    if (r < 0) return -1;
    if (crc_it)
        for (int y = 0; y < canvas->height; y++)
            *crc = crc_update(*crc, canvas->data[0] + (ptrdiff_t)y * canvas->linesize[0], (size_t)canvas->width * 4);
    t0 = os64_micros();
    r = sws_scale_frame(fast, canvas, decoded);
    *fast_us = os64_micros() - t0;
    return r < 0 ? -1 : 0;
}

// Decodes `path`'s first video stream in `mode`, converting each frame as
// `scale` says. Returns 0 or the exit code.
static int bench(const char *path, int mode, long limit, const Scale *scale, Result *res)
{
    os64_memset(res, 0, sizeof(*res));
    int64_t size = 0;
    uint8_t *data = read_whole(path, &size);
    if (!data) { os64_printf("avbench: cannot read %s\n", path); return 2; }

    // The tier, set before the decoder opens: a decoder chooses its
    // routines from the flags in force when it is opened.
    av_force_cpu_flags(-1);
    unsigned detected = (unsigned)av_get_cpu_flags();
    if (mode == MODE_C)
        av_force_cpu_flags(0);
    else if (mode == MODE_SSE)
        av_force_cpu_flags((int)(detected & ~(unsigned)(AV_CPU_FLAG_AVX | AV_CPU_FLAG_AVXSLOW |
                                                         AV_CPU_FLAG_AVX2 | AV_CPU_FLAG_FMA3 |
                                                         AV_CPU_FLAG_AVX512 | AV_CPU_FLAG_AVX512ICL)));
    flag_names(res->flags, sizeof(res->flags), (unsigned)av_get_cpu_flags());

    int rc = 2;
    Memory mem = { .data = data, .size = size };
    AVIOContext *io = NULL;
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    size_t cap = 4096, n = 0;
    int64_t *times = os64_malloc(cap * sizeof(int64_t));
    SwsContext *bilinear = NULL, *fast = NULL;
    uint8_t *pixels = NULL;
    AVFrame *canvas = NULL;
    Series s_bilinear = { 0 }, s_fast = { 0 }, s_decode = { 0 }, s_with_bilinear = { 0 }, s_with_fast = { 0 };
    bool warmed = false;
    uint32_t scale_crc = 0xFFFFFFFFu;
    enum { IO_BUFFER = 65536 };
    uint8_t *iobuf = av_malloc(IO_BUFFER);
    if (!pkt || !frame || !times || !iobuf) goto out;
    io = avio_alloc_context(iobuf, IO_BUFFER, 0, &mem, memory_read, NULL, memory_seek);
    if (!io) { av_free(iobuf); goto out; }
    fmt = avformat_alloc_context();
    if (!fmt) goto out;
    fmt->pb = io;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
        os64_printf("avbench: %s: not a file libav can read\n", path);
        goto out;
    }
    const AVCodec *codec = NULL;
    int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) { os64_printf("avbench: %s: no video stream libav decodes\n", path); goto out; }
    dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) < 0) goto out;
    dec->thread_count = 1;
    if (avcodec_open2(dec, codec, NULL) < 0) { os64_printf("avbench: cannot open the decoder\n"); goto out; }
    res->width = dec->width;
    res->height = dec->height;
    res->clip_fps = av_q2d(fmt->streams[stream]->avg_frame_rate);

    rc = 3;
    if (scale->on) {
        // The scalers are made after the tier is set, as the decoder is: the
        // scaler chooses its routines from the flags in force when its graph
        // is built.
        res->scale_width = scale->natural ? dec->width : scale->width;
        res->scale_height = scale->natural ? dec->height : scale->height;
        res->scale_exact = scale->exact;
        size_t bytes = (size_t)res->scale_width * 4 * (size_t)res->scale_height;
        pixels = os64_malloc_aligned(bytes, 64);
        bilinear = scaler_new(SWS_BILINEAR | (scale->exact ? SWS_BITEXACT | SWS_ACCURATE_RND : 0));
        fast = scaler_new(SWS_FAST_BILINEAR);
        canvas = pixels ? canvas_frame(pixels, res->scale_width, res->scale_height) : NULL;
        if (!canvas || !bilinear || !fast) { os64_printf("avbench: no memory for the conversion\n"); goto out; }
        os64_memset(pixels, 0, bytes);    // first touches out of the timings
    }

    crc_init();
    uint32_t crc = 0xFFFFFFFFu;
    int64_t total = 0;
    int flushing = 0;
    for (;;) {
        if (!flushing) {
            // Only the end of the file ends a run normally: any other read
            // error would otherwise flush, and report the frames decoded so
            // far as a whole run (which `all` would record as a result).
            int r = av_read_frame(fmt, pkt);
            if (r == AVERROR_EOF) flushing = 1;
            else if (r < 0) { os64_printf("avbench: %s: read error %d partway\n", path, r); goto out; }
            else if (pkt->stream_index != stream) { av_packet_unref(pkt); continue; }
        }
        // Only FFmpeg's own calls are timed. The CRC and the unref happen
        // between the clocks: the CRC walks every byte of every frame, and
        // at the fixture's -O0 it costs nearly as much as the decode.
        int64_t t0 = os64_micros();
        int s = avcodec_send_packet(dec, flushing ? NULL : pkt);
        int64_t dt = os64_micros() - t0;
        int64_t packet_bilinear = 0, packet_fast = 0;
        int got = 0;
        int r;
        for (;;) {
            t0 = os64_micros();
            r = avcodec_receive_frame(dec, frame);
            dt += os64_micros() - t0;
            if (r != 0)
                break;
            got = 1;
            if (limit <= 0 || res->frames < limit) {
                crc = crc_frame(crc, frame);
                res->frames++;
                if (scale->on) {
                    int64_t b_us, f_us;
                    // A scaler builds its graph at its first frame, once: that
                    // conversion is made untimed, then the frame is timed.
                    if (!warmed) {
                        uint32_t unused = 0;
                        if (convert(bilinear, fast, canvas, frame, false, &unused, &b_us, &f_us) < 0) {
                            os64_printf("avbench: the scaler refused %s\n",
                                        av_get_pix_fmt_name((enum AVPixelFormat)frame->format));
                            goto out;
                        }
                        warmed = true;
                    }
                    if (convert(bilinear, fast, canvas, frame, scale->exact, &scale_crc, &b_us, &f_us) < 0) {
                        os64_printf("avbench: conversion failed partway\n");
                        goto out;
                    }
                    if (series_push(&s_bilinear, b_us) < 0 || series_push(&s_fast, f_us) < 0) {
                        os64_printf("avbench: no memory for the times\n");
                        goto out;
                    }
                    packet_bilinear += b_us;
                    packet_fast += f_us;
                }
            }
            av_frame_unref(frame);
        }
        total += dt;
        // The packets that gave frames, the flush's included: each one's
        // decode alone, and with the conversions of what it gave.
        if (scale->on && got && (series_push(&s_decode, dt) < 0 ||
                                 series_push(&s_with_bilinear, dt + packet_bilinear) < 0 ||
                                 series_push(&s_with_fast, dt + packet_fast) < 0)) {
            os64_printf("avbench: no memory for the times\n");
            goto out;
        }
        // A decoder error fails the run before anything may end it normally,
        // the flush included (draining H.264's delayed pictures can fail):
        // EAGAIN (send more) and EOF (drained) are the decoder's two normal
        // answers to a receive, and to a send while flushing.
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) { os64_printf("avbench: decode error %d\n", r); goto out; }
        if (s < 0 && s != AVERROR(EAGAIN) && s != AVERROR_EOF) { os64_printf("avbench: decode error %d\n", s); goto out; }
        if (!flushing) {
            av_packet_unref(pkt);
            if (n == cap) {
                int64_t *bigger = os64_realloc(times, cap * 2 * sizeof(int64_t));
                if (!bigger) goto out;
                times = bigger;
                cap *= 2;
            }
            times[n++] = dt;
        }
        if ((flushing && !got) || (limit > 0 && res->frames >= limit)) break;
    }
    sort_us(times, n);
    res->crc = crc ^ 0xFFFFFFFFu;
    res->fps = total > 0 ? res->frames * 1e6 / (double)total : 0;
    res->worst_us = n ? times[n - 1] : 0;
    res->p99_us = n ? times[(size_t)(n * 0.99)] : 0;
    if (scale->on) {
        res->decode = series_cost(&s_decode);
        res->bilinear = series_cost(&s_bilinear);
        res->fast = series_cost(&s_fast);
        res->with_bilinear = series_cost(&s_with_bilinear);
        res->with_fast = series_cost(&s_with_fast);
        res->scale_crc = scale_crc ^ 0xFFFFFFFFu;
    }
    rc = 0;

out:
    os64_free(times);
    os64_free(s_bilinear.v);
    os64_free(s_fast.v);
    os64_free(s_decode.v);
    os64_free(s_with_bilinear.v);
    os64_free(s_with_fast.v);
    sws_free_context(&bilinear);
    sws_free_context(&fast);
    av_frame_free(&canvas);          // holds no buffer of FFmpeg's: the pixels are freed next
    if (pixels) os64_free(pixels);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
    os64_free(data);
    av_force_cpu_flags(-1);
    return rc;
}

static void report(const char *path, int mode, const Result *r)
{
    char line[512];
    // Whole numbers of tenths: libos64's formatter is integer-first.
    long fps10 = (long)(r->fps * 10 + 0.5), xrt100 = r->clip_fps > 0 ? (long)(r->fps / r->clip_fps * 100 + 0.5) : 0;
    long clip10 = (long)(r->clip_fps * 10 + 0.5);
    os64_snprintf(line, sizeof(line),
                  "avbench %s %dx%d@%ld.%ld mode %s flags [%s] frames %ld fps %ld.%ld x%ld.%02ld "
                  "worst %ld.%03ld ms p99 %ld.%03ld ms crc32 %08x",
                  path, r->width, r->height, clip10 / 10, clip10 % 10, k_mode_name[mode], r->flags,
                  r->frames, fps10 / 10, fps10 % 10, xrt100 / 100, xrt100 % 100,
                  (long)(r->worst_us / 1000), (long)(r->worst_us % 1000),
                  (long)(r->p99_us / 1000), (long)(r->p99_us % 1000), r->crc);
    os64_printf("%s\n", line);
    if (mode == MODE_C)
        os64_printf("avbench: -c forces FFmpeg's C routines; inline assembly chosen at compile time stays in\n");
    os64_serial_log(line);
    if (r->scale_width == 0)
        return;
    // Average/worst, in ms: a frame's conversions; a packet's decode, alone
    // and with them (the header says which packets).
#define MS(c) (long)((c).avg_us / 1000), (long)((c).avg_us % 1000), (long)((c).worst_us / 1000), (long)((c).worst_us % 1000)
    const char *bilinear = r->scale_exact ? "bilinear-exact" : "bilinear";
    os64_snprintf(line, sizeof(line),
                  "avbench %s to %dx%d mode %s ms avg/worst: decode %ld.%03ld/%ld.%03ld "
                  "%s %ld.%03ld/%ld.%03ld fast %ld.%03ld/%ld.%03ld "
                  "decode+%s %ld.%03ld/%ld.%03ld decode+fast %ld.%03ld/%ld.%03ld",
                  path, r->scale_width, r->scale_height, k_mode_name[mode], MS(r->decode), bilinear,
                  MS(r->bilinear), MS(r->fast), bilinear, MS(r->with_bilinear), MS(r->with_fast));
#undef MS
    os64_printf("%s\n", line);
    os64_serial_log(line);
}

static void report_exact(const Result *r)
{
    char line[96];
    os64_snprintf(line, sizeof(line), "avbench: bit-exact bilinear to %dx%d crc32 %08x", r->scale_width,
                  r->scale_height, r->scale_crc);
    os64_printf("%s\n", line);
    os64_serial_log(line);
}

// A number of 1 to 16384 at *s, which is moved past it; -1 when there is
// none or it is out of range (checked as each digit arrives, so a long
// run of digits cannot overflow).
static long parse_side(const char **s)
{
    long v = 0;
    const char *p = *s;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        if (v > 16384) return -1;
    }
    *s = p;
    return v < 1 ? -1 : v;
}

// "WxH" or "natural" into `scale`, the last one given winning. -1 when it
// is neither.
static int parse_size(const char *s, Scale *scale)
{
    scale->on = true;
    if (os64_streq(s, "natural")) {
        scale->natural = true;
        return 0;
    }
    long w = parse_side(&s);
    if (w < 0 || *s++ != 'x') return -1;
    long h = parse_side(&s);
    if (h < 0 || *s != '\0') return -1;
    scale->natural = false;
    scale->width = (int)w;
    scale->height = (int)h;
    return 0;
}

static int parse_hex(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int digits = 0;
    for (; *s; s++, digits++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10
              : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (d < 0 || digits == 8) return -1;
        v = v << 4 | (uint32_t)d;
    }
    if (!digits) return -1;
    *out = v;
    return 0;
}

static int ends_with_mp4(const char *name)
{
    size_t n = os64_strlen(name);
    return n > 4 && os64_strcmp(name + n - 4, ".mp4") == 0;
}

// Every .mp4 in `dir`, every mode, `runs` times, converted as `scale` says;
// one line a run appended to RESULTS. Names are sorted so two machines'
// files line up.
static int bench_all(const char *dir, int runs, const Scale *scale)
{
    enum { MAX_FILES = 64 };
    static char names[MAX_FILES][OS64_DIRENT_NAME_MAX + 1];
    int count = 0;
    int64_t d = os64_opendir(dir);
    if (d < 0) { os64_printf("avbench: cannot open %s\n", dir); return 2; }
    os64_dirent_t e;
    while (count < MAX_FILES && os64_readdir((int32_t)d, &e) == 1)
        if (ends_with_mp4(e.name))
            os64_strcopy(names[count++], sizeof(names[0]), e.name);
    os64_close((int32_t)d);
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && os64_strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[OS64_DIRENT_NAME_MAX + 1];
            os64_strcopy(t, sizeof(t), names[j]);
            os64_strcopy(names[j], sizeof(t), names[j - 1]);
            os64_strcopy(names[j - 1], sizeof(t), t);
        }
    if (!count) { os64_printf("avbench: no .mp4 files in %s\n", dir); return 2; }

    int64_t out = os64_open(RESULTS, "a");
    if (out < 0) os64_printf("avbench: cannot append to %s; results go to the screen only\n", RESULTS);
    int worst = 0;
    for (int i = 0; i < count; i++)
        for (int mode = MODE_AUTO; mode <= MODE_C; mode++)
            for (int run = 1; run <= runs; run++) {
                char path[512];
                os64_snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
                Result r;
                int rc = bench(path, mode, 0, scale, &r);
                if (rc) { worst = rc; continue; }
                report(path, mode, &r);
                if (out >= 0) {
                    char line[800];
                    long fps10 = (long)(r.fps * 10 + 0.5);
                    int len = os64_snprintf(line, sizeof(line),
                                            "%s %dx%d mode %s run %d frames %ld fps %ld.%ld worst_us %ld p99_us %ld crc32 %08x flags [%s]",
                                            names[i], r.width, r.height, k_mode_name[mode], run, r.frames,
                                            fps10 / 10, fps10 % 10, (long)r.worst_us, (long)r.p99_us, r.crc, r.flags);
                    if (r.scale_width && len > 0 && (size_t)len < sizeof(line))
                        len += os64_snprintf(line + len, sizeof(line) - (size_t)len,
                                             " to %dx%d us avg/worst bilinear %ld/%ld fast %ld/%ld "
                                             "decode+bilinear %ld/%ld decode+fast %ld/%ld",
                                             r.scale_width, r.scale_height, (long)r.bilinear.avg_us,
                                             (long)r.bilinear.worst_us, (long)r.fast.avg_us, (long)r.fast.worst_us,
                                             (long)r.with_bilinear.avg_us, (long)r.with_bilinear.worst_us,
                                             (long)r.with_fast.avg_us, (long)r.with_fast.worst_us);
                    if (len > 0 && (size_t)len < sizeof(line) - 1) {
                        line[len++] = '\n';
                        os64_write((int32_t)out, line, (size_t)len);
                    }
                }
            }
    if (out >= 0) {
        os64_close((int32_t)out);
        os64_printf("avbench: results appended to %s\n", RESULTS);
    }
    return worst;
}

static int usage(void)
{
    os64_printf("usage: avbench [-c | -s] [-n frames] [-k crc] [-x WxH|natural [-e] [-K crc]] file\n"
                "       avbench all <directory> [runs] [-x WxH|natural]\n");
    return 1;
}

int main(int argc, char **argv)
{
    av_log_set_callback(on_log);
    Scale scale = { 0 };
    if (argc >= 3 && os64_streq(argv[1], "all")) {
        int runs = 3;
        for (int i = 3; i < argc; i++) {
            if (os64_streq(argv[i], "-x") && i + 1 < argc) {
                if (parse_size(argv[++i], &scale) < 0) return usage();
            } else if (argv[i][0] != '-') {
                runs = (int)os64_atoi(argv[i]);
            } else {
                return usage();
            }
        }
        return bench_all(argv[2], runs > 0 ? runs : 3, &scale);
    }
    int mode = MODE_AUTO;
    long limit = 0;
    int have_crc = 0;
    uint32_t want = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (os64_streq(argv[i], "-c")) mode = MODE_C;
        else if (os64_streq(argv[i], "-s")) mode = MODE_SSE;
        else if (os64_streq(argv[i], "-n") && i + 1 < argc) limit = (long)os64_atoi(argv[++i]);
        else if (os64_streq(argv[i], "-k") && i + 1 < argc) {
            if (parse_hex(argv[++i], &want) < 0) return usage();
            have_crc = 1;
        } else if (os64_streq(argv[i], "-x") && i + 1 < argc) {
            if (parse_size(argv[++i], &scale) < 0) return usage();
        } else if (os64_streq(argv[i], "-e")) {
            scale.exact = true;
        } else if (os64_streq(argv[i], "-K") && i + 1 < argc) {
            if (parse_hex(argv[++i], &scale.want_crc) < 0) return usage();
            scale.have_crc = true;
        } else if (argv[i][0] != '-' && !path) path = argv[i];
        else return usage();
    }
    if (!path || ((scale.exact || scale.have_crc) && !scale.on) || (scale.have_crc && !scale.exact)) return usage();
    Result r;
    int rc = bench(path, mode, limit, &scale, &r);
    if (rc) return rc;
    report(path, mode, &r);
    if (scale.exact) report_exact(&r);
    if (have_crc && r.crc != want) {
        os64_printf("avbench: crc32 %08x, expected %08x\n", r.crc, want);
        return 4;
    }
    if (scale.have_crc && r.scale_crc != scale.want_crc) {
        os64_printf("avbench: converted crc32 %08x, expected %08x\n", r.scale_crc, scale.want_crc);
        return 4;
    }
    return have_crc || scale.have_crc ? (int)AVBENCH_CRC_OK : 0;
}
