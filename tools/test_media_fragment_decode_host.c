// libmedia's fragment parser and track buffer on real streams, decoded: what
// they hand a player must decode, bit for bit, as the host's ffmpeg decodes
// the same files, and with the same presentation times (the edit list
// applied as FFmpeg applies it).
//
// The streams (made by tools/test_media_fragment_decode_host.sh with the
// host's ffmpeg): a picture in two qualities and a sound, each an init
// segment and media segments the way a streaming site serves them. Each
// scenario is fed under every cut to the parser, whose sink is a track
// buffer (os64/media_track.h); then a cursor reads the track from the start
// of what is buffered, in decode order and across ranges, and the pinned
// FFmpeg's avcodec decodes each frame, opened from the generation the
// buffer holds (avcC as extradata, the AudioSpecificConfig likewise). A new
// generation is a new decoder. A PRELOAD frame (AAC's priming, written as
// an edit, lands before the append window's start at 0) is DECODED and its
// output discarded, which is what the host's ffmpeg does: dropping it
// unread instead changes the next frame's sound, because AAC overlaps each
// frame's transform with the one before (MEDIA_SOURCE.md § 4, preroll).
//
//   test_media_fragment_decode <media directory>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>

#include "os64/media_fragment.h"
#include "os64/media_track.h"

static unsigned long s_checks, s_failed;

static void check(bool ok, const char *fmt, ...)
{
    s_checks++;
    if (!ok && ++s_failed <= 30) {
        va_list ap;
        va_start(ap, fmt);
        printf("FAIL: ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
}

typedef struct { uint8_t *d; size_t n; } blob_t;
static const char *s_dir;

static blob_t load(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    blob_t b = { .n = (size_t)ftell(f) };
    fseek(f, 0, SEEK_SET);
    b.d = malloc(b.n);
    if (fread(b.d, 1, b.n, f) != b.n) exit(1);
    fclose(f);
    return b;
}

// framemd5: one line a frame, "stream, dts, pts, duration, size, md5".
typedef struct { char md5[33]; long long pts; } ref_line_t;
typedef struct { ref_line_t *l; int n; } ref_t;

static ref_t load_ref(const char *name)
{
    char path[1024], line[512];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    ref_t r = { .l = malloc(8192 * sizeof(ref_line_t)) };
    while (fgets(line, sizeof(line), f) && r.n < 8192) {
        if (line[0] == '#') continue;
        int stream; long long dts, pts, dur; long size;
        char md5[64];
        if (sscanf(line, "%d, %lld, %lld, %lld, %ld, %63s", &stream, &dts, &pts, &dur, &size, md5) == 6) {
            memcpy(r.l[r.n].md5, md5, 32);
            r.l[r.n].md5[32] = 0;
            r.l[r.n].pts = pts;
            r.n++;
        }
    }
    fclose(f);
    return r;
}

// The parser's sink: the track buffer.
static void on_init(void *c, const os64_media_fragment_init_t *init)
{
    os64_media_track_rule_t r = os64_media_buffer_init_segment(c, init);
    check(r == OS64_MEDIA_TRACK_OK, "the buffer refused an init segment: %s", os64_media_track_rule_name(r));
}

static void on_frame(void *c, const os64_media_fragment_frame_t *f)
{
    os64_media_track_rule_t r = os64_media_buffer_frame(c, f);
    check(r == OS64_MEDIA_TRACK_OK, "the buffer refused a frame: %s", os64_media_track_rule_name(r));
}

static void *m_alloc(void *c, size_t n) { (void)c; return malloc(n); }
static void m_release(void *c, void *b) { (void)c; free(b); }

enum { CUT_WHOLE, CUT_RANDOM, CUT_HEADERS, CUT_BYTE };
static const char *const kCut[] = { "whole", "random pieces", "inside every header", "one byte at a time" };

static size_t next_cut(const blob_t *b, size_t at, int cut, uint64_t *rng)
{
    size_t left = b->n - at;
    if (cut == CUT_WHOLE) return left;
    if (cut == CUT_BYTE) return 1;
    if (cut == CUT_RANDOM) {
        *rng ^= *rng << 13; *rng ^= *rng >> 7; *rng ^= *rng << 17;
        size_t n = 1 + (size_t)(*rng % 65536);
        return n < left ? n : left;
    }
    size_t pos = 0;
    while (pos + 8 <= b->n) {
        uint32_t size = (uint32_t)b->d[pos] << 24 | b->d[pos + 1] << 16 | b->d[pos + 2] << 8 | b->d[pos + 3];
        for (size_t c = pos + 3; c <= pos + 11; c += 8)
            if (c > at && c < b->n) return c - at;
        if (size < 8) break;
        pos += size;
    }
    return left;
}

static void md5_of(const AVFrame *fr, bool picture, char out[33])
{
    struct AVMD5 *m = av_md5_alloc();
    av_md5_init(m);
    if (picture) {
        for (int p = 0; p < 3; p++) {
            int w = p ? fr->width / 2 : fr->width, h = p ? fr->height / 2 : fr->height;
            for (int y = 0; y < h; y++) av_md5_update(m, fr->data[p] + (size_t)y * fr->linesize[p], w);
        }
    } else {
        int ch = fr->ch_layout.nb_channels;
        for (int i = 0; i < fr->nb_samples; i++)
            for (int c = 0; c < ch; c++) av_md5_update(m, fr->extended_data[c] + (size_t)i * 4, 4);
    }
    uint8_t d[16];
    av_md5_final(m, d);
    av_free(m);
    for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void compare_out(AVFrame *fr, bool picture, const ref_t *r, int out, int64_t per_second, int *bad_md5, int *bad_pts)
{
    char h[33];
    md5_of(fr, picture, h);
    // The reference's times are in 1/per_second; the decoder's in nanoseconds.
    long long want_pts = (fr->pts * per_second + (fr->pts >= 0 ? 500000000 : -500000000)) / 1000000000;
    if (out >= r->n || strcmp(h, r->l[out].md5)) (*bad_md5)++;
    else if (want_pts != r->l[out].pts) (*bad_pts)++;
}

// Read the track through a cursor from the start of what is buffered and
// decode it; compare output n with want[n] (want2 from `switch_at` on).
static void decode_and_compare(const char *name, const char *cut, os64_media_buffer_t *b, int kind, enum AVCodecID id,
                               const ref_t *want, const ref_t *want2, int switch_at, int expect, int64_t per_second)
{
    os64_media_track_t *t = os64_media_buffer_track(b, kind);
    os64_media_time_range_t first;
    check(t && os64_media_track_ranges(t, &first, 1) >= 1, "%s, %s: nothing buffered", name, cut);
    os64_media_time_range_t all[8];
    uint32_t nranges = t ? os64_media_track_ranges(t, all, 8) : 0;
    os64_media_track_cursor_t cursor;
    check(t && os64_media_track_seek(t, first.start, &cursor) == OS64_MEDIA_TRACK_FRAME, "%s, %s: no seek", name, cut);
    const AVCodec *codec = avcodec_find_decoder(id);
    bool picture = id == AV_CODEC_ID_H264;
    AVCodecContext *cc = 0;
    uint32_t generation = 0;
    int out = 0, bad_md5 = 0, bad_pts = 0, reopens = 0, preroll = 0, resyncs = 0;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    os64_media_track_frame_t f;
    for (;;) {
        int r = t ? os64_media_track_next(t, &cursor, &f) : OS64_MEDIA_TRACK_WAIT;
        if (r == OS64_MEDIA_TRACK_RESYNC) resyncs++;
        bool last = r != OS64_MEDIA_TRACK_FRAME;
        if ((last || f.generation != generation) && cc) {
            avcodec_send_packet(cc, 0);
            while (avcodec_receive_frame(cc, fr) == 0) {
                if (preroll > 0) { preroll--; av_frame_unref(fr); continue; }
                compare_out(fr, picture, out < switch_at ? want : want2, out, per_second, &bad_md5, &bad_pts);
                out++;
                av_frame_unref(fr);
            }
            avcodec_free_context(&cc);
            os64_media_track_generation_drop(t, generation);
        }
        if (last) break;
        if (f.generation != generation) {
            generation = f.generation;
            os64_media_track_generation_t g;
            check(os64_media_track_generation_hold(t, generation, &g), "%s, %s: generation %u gone", name, cut, generation);
            cc = avcodec_alloc_context3(codec);
            cc->extradata = av_mallocz(g.entry[0].config_size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(cc->extradata, g.entry[0].config, g.entry[0].config_size);
            cc->extradata_size = (int)g.entry[0].config_size;
            cc->width = g.entry[0].width;
            cc->height = g.entry[0].height;
            cc->pkt_timebase = (AVRational){ 1, 1000000000 };
            if (avcodec_open2(cc, codec, 0) < 0) { fprintf(stderr, "decoder open failed\n"); exit(1); }
            reopens++;
        }
        // A sound decoder hands its frames back in the order they went in, so
        // counting the preload frames is enough (a picture is never one).
        if (f.flags & OS64_MEDIA_FRAME_PRELOAD)
            preroll++;
        // avcodec copies a packet it does not own; the player's hold covers the call.
        uint8_t *copy = av_mallocz(f.size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(copy, f.slab->data + f.offset, f.size);
        pkt->data = copy;
        pkt->size = (int)f.size;
        pkt->dts = f.dts;
        pkt->pts = f.pts;
        if (avcodec_send_packet(cc, pkt) < 0) bad_md5++;
        av_free(copy);
        os64_media_track_frame_done(&f);
        while (avcodec_receive_frame(cc, fr) == 0) {
            if (preroll > 0) { preroll--; av_frame_unref(fr); continue; }
            compare_out(fr, picture, out < switch_at ? want : want2, out, per_second, &bad_md5, &bad_pts);
            out++;
            av_frame_unref(fr);
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    check(nranges == 1, "%s, %s: %u buffered ranges, want one", name, cut, nranges);
    check(resyncs == 0, "%s, %s: the cursor answered RESYNC", name, cut);
    check(out == expect, "%s, %s: %d frames decoded, want %d", name, cut, out, expect);
    check(bad_md5 == 0, "%s, %s: %d frames differ from the host's decode (of %d)", name, cut, bad_md5, out);
    check(bad_pts == 0, "%s, %s: %d presentation times differ from the host's (of %d)", name, cut, bad_pts, out);
    printf("  %-44s %-20s %4d frames, %u range%s, %d decoder opens, %d MD5 and %d time mismatches\n", name, cut, out,
           t ? t->range_count : 0, t && t->range_count == 1 ? " " : "s", reopens, bad_md5, bad_pts);
}

static void scenario(const char *name, blob_t **appends, int n, int kind, enum AVCodecID id, const ref_t *want,
                     const ref_t *want2, int switch_at, int expect, int64_t per_second, int cuts)
{
    for (int cut = 0; cut < cuts; cut++) {
        os64_media_track_memory_t tmem = { m_alloc, m_release, 0 };
        os64_media_buffer_t b;
        os64_media_buffer_init(&b, &tmem);
        os64_media_fragment_memory_t mem = { m_alloc, m_release, 0, 0, 0 };
        os64_media_fragment_sink_t sink = { on_init, on_frame, &b };
        os64_media_fragment_t p;
        os64_media_fragment_init(&p, &mem, &sink);
        uint64_t rng = 0x9E3779B97F4A7C15ull;
        int rule = 0;
        for (int a = 0; a < n && rule == 0; a++)
            for (size_t at = 0; at < appends[a]->n;) {
                size_t k = next_cut(appends[a], at, cut, &rng);
                if (os64_media_fragment_append(&p, appends[a]->d + at, k) < 0) {
                    rule = os64_media_fragment_error(&p);
                    break;
                }
                at += k;
            }
        os64_media_fragment_destroy(&p);
        check(rule == 0, "%s, %s: the parser refused it: %s", name, kCut[cut],
              os64_media_fragment_rule_name((os64_media_fragment_rule_t)rule));
        decode_and_compare(name, kCut[cut], &b, kind, id, want, want2, switch_at, expect, per_second);
        os64_media_buffer_destroy(&b);
    }
}

int main(int argc, char **argv)
{
    s_dir = argc > 1 ? argv[1] : ".";
    av_log_set_level(AV_LOG_ERROR);
    blob_t init[3], seg[3][8];
    char name[64];
    for (int q = 0; q < 3; q++) {
        snprintf(name, sizeof(name), "init-%d.m4s", q);
        init[q] = load(name);
        for (int i = 1; i <= (q == 2 ? 7 : 6); i++) {
            snprintf(name, sizeof(name), "seg-%d-%d.m4s", q, i);
            seg[q][i] = load(name);
        }
    }
    ref_t r0 = load_ref("ref-0.md5"), r1 = load_ref("ref-1.md5"), r2 = load_ref("ref-2.md5");
    // framemd5's picture times are in frames (1/30 s), its sound's in samples (1/48000).
    const int V = OS64_MEDIA_FRAGMENT_VIDEO, A = OS64_MEDIA_FRAGMENT_AUDIO;

    blob_t *a[16];
    int n;
    n = 0; a[n++] = &init[0]; for (int i = 1; i <= 6; i++) a[n++] = &seg[0][i];
    scenario("720p, segments 1-6", a, n, V, AV_CODEC_ID_H264, &r0, &r0, 1 << 30, r0.n, 30, 4);
    n = 0; a[n++] = &init[0]; for (int i = 1; i <= 3; i++) a[n++] = &seg[0][i];
    a[n++] = &init[1]; for (int i = 4; i <= 6; i++) a[n++] = &seg[1][i];
    scenario("quality switch 720p to 360p after segment 3", a, n, V, AV_CODEC_ID_H264, &r0, &r1, 180, 360, 30, 3);
    n = 0; a[n++] = &init[0];
    { int order[] = { 4, 5, 1, 2, 3, 6 }; for (int i = 0; i < 6; i++) a[n++] = &seg[0][order[i]]; }
    scenario("appended out of order (4, 5, 1, 2, 3, 6)", a, n, V, AV_CODEC_ID_H264, &r0, &r0, 1 << 30, r0.n, 30, 3);
    n = 0; a[n++] = &init[2]; for (int i = 1; i <= 7; i++) a[n++] = &seg[2][i];
    scenario("AAC, segments 1-7", a, n, A, AV_CODEC_ID_AAC, &r2, &r2, 1 << 30, r2.n, 48000, 3);

    for (int q = 0; q < 3; q++) {
        free(init[q].d);
        for (int i = 1; i <= (q == 2 ? 7 : 6); i++) free(seg[q][i].d);
    }
    free(r0.l); free(r1.l); free(r2.l);
    printf("media fragment decode: %lu checks, %lu failed\n", s_checks, s_failed);
    return s_failed != 0;
}
