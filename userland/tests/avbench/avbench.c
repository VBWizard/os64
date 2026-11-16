// avbench — how fast libav.so decodes video on this machine, and whether it
// decodes the right pictures.
//
//   avbench [-c | -s] [-n frames] [-k crc] file
//   avbench all <directory> [runs]
//
// The file is read whole into memory first, so the disk is not in the
// figure, and FFmpeg gets its bytes through the read and seek callbacks, as
// every caller does. The first video stream is decoded on the calling
// thread (avbench starts none, so `env PINCPU=<n>` pins all of it), sound
// ignored, and each packet's send-and-receive is timed with micros().
//
//   (neither)  FFmpeg's own choice of routines for this CPU
//   -s         the SSE tiers: AVX, AVX2 and FMA3 masked out of what FFmpeg
//              detected (the host measurement's tier 2)
//   -c         the C routines: av_force_cpu_flags(0). Not the same as a
//              library built without assembly: the inline assembly FFmpeg
//              chooses at compile time stays in, and the output says so
//   -n frames  stop after that many frames
//   -k crc     the CRC the frames must give (hex); a different one exits 4
//
// It prints the picture size and rate, the CPU flags FFmpeg reports it is
// using, frames decoded, frames per second and that as a multiple of the
// clip's own rate, the worst frame and the 99th percentile in milliseconds,
// and a CRC-32 (zlib's) over every decoded frame's planes, visible rows and
// columns only. H.264 decoding is bit-exact, so the CRC is the same for
// every mode, on every machine, and on the host (the host's tool computes
// exactly this).
//
// `all` runs every .mp4 in the directory in each of the three modes, `runs`
// times each (3 if absent), and appends one line a run to /home/avbench.txt,
// which is the file to send back.
//
// Exit: 0 decoded to the end, 1 usage, 2 cannot read or open the file,
// 3 decode failed, 4 CRC differs from -k. With -k, a match exits
// AVBENCH_CRC_OK instead of 0: testrun's badge, a word spelled in hex, so a
// program that ended for any other reason cannot pass for this one.

#include "os64/os64.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/cpu.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"

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

typedef struct {
    int width, height;
    double clip_fps, fps;
    long frames;
    int64_t worst_us, p99_us;
    uint32_t crc;
    char flags[160];
} Result;

// Decodes `path`'s first video stream in `mode`. Returns 0 or the exit code.
static int bench(const char *path, int mode, long limit, Result *res)
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

    crc_init();
    uint32_t crc = 0xFFFFFFFFu;
    int64_t total = 0;
    int flushing = 0;
    rc = 3;
    for (;;) {
        if (!flushing) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) flushing = 1;
            else if (pkt->stream_index != stream) { av_packet_unref(pkt); continue; }
        }
        int64_t t0 = os64_micros();
        int s = avcodec_send_packet(dec, flushing ? NULL : pkt);
        int got = 0;
        int r;
        while ((r = avcodec_receive_frame(dec, frame)) == 0) {
            got = 1;
            if (limit <= 0 || res->frames < limit) {
                crc = crc_frame(crc, frame);
                res->frames++;
            }
            av_frame_unref(frame);
        }
        int64_t dt = os64_micros() - t0;
        total += dt;
        if (!flushing) {
            av_packet_unref(pkt);
            if (s < 0 && s != AVERROR(EAGAIN)) { os64_printf("avbench: decode error %d\n", s); goto out; }
            if (n == cap) {
                int64_t *bigger = os64_realloc(times, cap * 2 * sizeof(int64_t));
                if (!bigger) goto out;
                times = bigger;
                cap *= 2;
            }
            times[n++] = dt;
        }
        if ((flushing && !got) || (limit > 0 && res->frames >= limit)) break;
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) { os64_printf("avbench: decode error %d\n", r); goto out; }
    }
    sort_us(times, n);
    res->crc = crc ^ 0xFFFFFFFFu;
    res->fps = total > 0 ? res->frames * 1e6 / (double)total : 0;
    res->worst_us = n ? times[n - 1] : 0;
    res->p99_us = n ? times[(size_t)(n * 0.99)] : 0;
    rc = 0;

out:
    os64_free(times);
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

// Every .mp4 in `dir`, every mode, `runs` times; one line a run appended to
// RESULTS. Names are sorted so two machines' files line up.
static int bench_all(const char *dir, int runs)
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
                int rc = bench(path, mode, 0, &r);
                if (rc) { worst = rc; continue; }
                report(path, mode, &r);
                if (out >= 0) {
                    char line[600];
                    long fps10 = (long)(r.fps * 10 + 0.5);
                    int len = os64_snprintf(line, sizeof(line),
                                            "%s %dx%d mode %s run %d frames %ld fps %ld.%ld worst_us %ld p99_us %ld crc32 %08x flags [%s]\n",
                                            names[i], r.width, r.height, k_mode_name[mode], run, r.frames,
                                            fps10 / 10, fps10 % 10, (long)r.worst_us, (long)r.p99_us, r.crc, r.flags);
                    os64_write((int32_t)out, line, (size_t)len);
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
    os64_printf("usage: avbench [-c | -s] [-n frames] [-k crc] file\n"
                "       avbench all <directory> [runs]\n");
    return 1;
}

int main(int argc, char **argv)
{
    av_log_set_callback(on_log);
    if (argc >= 3 && os64_streq(argv[1], "all")) {
        int runs = argc >= 4 ? (int)os64_atoi(argv[3]) : 3;
        return bench_all(argv[2], runs > 0 ? runs : 3);
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
        } else if (argv[i][0] != '-' && !path) path = argv[i];
        else return usage();
    }
    if (!path) return usage();
    Result r;
    int rc = bench(path, mode, limit, &r);
    if (rc) return rc;
    report(path, mode, &r);
    if (have_crc && r.crc != want) {
        os64_printf("avbench: crc32 %08x, expected %08x\n", r.crc, want);
        return 4;
    }
    return have_crc ? (int)AVBENCH_CRC_OK : 0;
}
