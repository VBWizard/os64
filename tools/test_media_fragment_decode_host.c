// libmedia's fragment parser on real streams, decoded: what the parser hands
// out must decode, bit for bit, as the host's ffmpeg decodes the same files,
// and with the same presentation times (the edit list applied as FFmpeg
// applies it).
//
// The streams (made by tools/test_media_fragment_decode_host.sh with the
// host's ffmpeg): a picture in two qualities and a sound, each an init
// segment and media segments the way a streaming site serves them. Each
// scenario is fed under every cut; the frames are copied out as a track
// buffer would keep them, taken in decode order, and decoded by the pinned
// FFmpeg's avcodec configured from the init segment (avcC as extradata, the
// AudioSpecificConfig likewise). A new init generation is a new decoder.
// A frame presented before time zero (AAC's priming, written as an edit) is
// DECODED and its output discarded, which is what the host's ffmpeg does:
// dropping it unread instead changes the next frame's sound, because AAC
// overlaps each frame's transform with the one before (MEDIA_SOURCE.md § 4,
// preroll).
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

// Frames as a track buffer would keep them: their own copy of the bytes.
typedef struct {
    uint8_t *data; int size;
    int64_t dts, pts;
    uint32_t generation, entry, timescale;
} kept_t;

typedef struct {
    kept_t *f; int n, cap;
    // each generation's configuration, copied at its init (good until the next)
    uint8_t *config[16]; int config_size[16]; int width[16], height[16];
    int kind;
} store_t;

static void on_init(void *c, const os64_media_fragment_init_t *init)
{
    store_t *s = c;
    uint32_t g = init->generation;
    if (g >= 16) return;
    const os64_media_fragment_entry_t *e = &init->track[0].entry[0];
    s->config[g] = av_mallocz(e->config_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(s->config[g], e->config, e->config_size);
    s->config_size[g] = (int)e->config_size;
    s->width[g] = e->width;
    s->height[g] = e->height;
    s->kind = init->track[0].kind;
}

static void on_frame(void *c, const os64_media_fragment_frame_t *f)
{
    store_t *s = c;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->f = realloc(s->f, s->cap * sizeof(kept_t));
    }
    kept_t *k = &s->f[s->n++];
    k->data = malloc(f->size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(k->data, f->slab->data + f->offset, f->size);
    memset(k->data + f->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    k->size = (int)f->size;
    k->dts = f->dts;
    k->pts = f->pts;
    k->generation = f->generation;
    k->entry = f->entry;
    k->timescale = f->timescale;
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

static int cmp_dts(const void *a, const void *b)
{
    const kept_t *x = a, *y = b;
    return x->dts < y->dts ? -1 : x->dts > y->dts;
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

// Decode the store in decode order; compare output n with want[n] (want2 from
// `switch_at` on), MD5 and presentation time (`pts_scale` turns the track's
// timescale into the reference's).
static void decode_and_compare(const char *name, const char *cut, store_t *s, enum AVCodecID id,
                               const ref_t *want, const ref_t *want2, int switch_at, int expect, double pts_scale)
{
    qsort(s->f, s->n, sizeof(kept_t), cmp_dts);
    const AVCodec *codec = avcodec_find_decoder(id);
    AVCodecContext *cc = 0;
    uint32_t generation = 0;
    int out = 0, bad_md5 = 0, bad_pts = 0, reopens = 0, preroll = 0;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    for (int i = 0; i <= s->n; i++) {
        bool last = i == s->n;
        if ((last || s->f[i].generation != generation) && cc) {
            avcodec_send_packet(cc, 0);
            while (avcodec_receive_frame(cc, fr) == 0) {
                if (preroll > 0) {
                    preroll--;
                    av_frame_unref(fr);
                    continue;
                }
                const ref_t *r = out < switch_at ? want : want2;
                char h[33];
                md5_of(fr, id == AV_CODEC_ID_H264, h);
                if (out >= r->n || strcmp(h, r->l[out].md5)) bad_md5++;
                else if ((long long)((double)fr->pts * pts_scale + 0.5) != r->l[out].pts) bad_pts++;
                out++;
                av_frame_unref(fr);
            }
            avcodec_free_context(&cc);
        }
        if (last) break;
        if (s->f[i].generation != generation) {
            generation = s->f[i].generation;
            cc = avcodec_alloc_context3(codec);
            cc->extradata = av_mallocz(s->config_size[generation] + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(cc->extradata, s->config[generation], s->config_size[generation]);
            cc->extradata_size = s->config_size[generation];
            cc->width = s->width[generation];
            cc->height = s->height[generation];
            cc->pkt_timebase = (AVRational){ 1, (int)s->f[i].timescale };
            if (avcodec_open2(cc, codec, 0) < 0) { fprintf(stderr, "decoder open failed\n"); exit(1); }
            reopens++;
        }
        // A sound decoder hands its frames back in the order they went in, so
        // counting is enough; no picture in these streams is before zero (a
        // picture decoder reorders, and would need its frames matched by time).
        if (id == AV_CODEC_ID_AAC && s->f[i].pts < 0)
            preroll++;
        pkt->data = s->f[i].data;
        pkt->size = s->f[i].size;
        pkt->dts = s->f[i].dts;
        pkt->pts = s->f[i].pts;
        if (avcodec_send_packet(cc, pkt) < 0) bad_md5++;
        while (avcodec_receive_frame(cc, fr) == 0) {
            if (preroll > 0) {          // the output of a frame before time zero
                preroll--;
                av_frame_unref(fr);
                continue;
            }
            const ref_t *r = out < switch_at ? want : want2;
            char h[33];
            md5_of(fr, id == AV_CODEC_ID_H264, h);
            if (out >= r->n || strcmp(h, r->l[out].md5)) bad_md5++;
            else if ((long long)((double)fr->pts * pts_scale + 0.5) != r->l[out].pts) bad_pts++;
            out++;
            av_frame_unref(fr);
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    check(out == expect, "%s, %s: %d frames decoded, want %d", name, cut, out, expect);
    check(bad_md5 == 0, "%s, %s: %d frames differ from the host's decode (of %d)", name, cut, bad_md5, out);
    check(bad_pts == 0, "%s, %s: %d presentation times differ from the host's (of %d)", name, cut, bad_pts, out);
    printf("  %-44s %-20s %4d frames, %d decoder opens, %d MD5 and %d time mismatches\n", name, cut, out, reopens,
           bad_md5, bad_pts);
}

static void scenario(const char *name, blob_t **appends, int n, enum AVCodecID id, const ref_t *want,
                     const ref_t *want2, int switch_at, int expect, double pts_scale, int cuts)
{
    for (int cut = 0; cut < cuts; cut++) {
        store_t s = { 0 };
        os64_media_fragment_memory_t mem = { m_alloc, m_release, 0, 0, 0 };
        os64_media_fragment_sink_t sink = { on_init, on_frame, &s };
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
        decode_and_compare(name, kCut[cut], &s, id, want, want2, switch_at, expect, pts_scale);
        for (int i = 0; i < s.n; i++) free(s.f[i].data);
        free(s.f);
        for (int g = 0; g < 16; g++) av_free(s.config[g]);
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
    // framemd5's picture times are in frames (1/30 s); the tracks' in 1/15360.
    double picture_scale = 30.0 / 15360.0;

    blob_t *a[16];
    int n;
    n = 0; a[n++] = &init[0]; for (int i = 1; i <= 6; i++) a[n++] = &seg[0][i];
    scenario("720p, segments 1-6", a, n, AV_CODEC_ID_H264, &r0, &r0, 1 << 30, r0.n, picture_scale, 4);
    n = 0; a[n++] = &init[0]; for (int i = 1; i <= 3; i++) a[n++] = &seg[0][i];
    a[n++] = &init[1]; for (int i = 4; i <= 6; i++) a[n++] = &seg[1][i];
    scenario("quality switch 720p to 360p after segment 3", a, n, AV_CODEC_ID_H264, &r0, &r1, 180, 360, picture_scale, 3);
    n = 0; a[n++] = &init[0];
    { int order[] = { 4, 5, 1, 2, 3, 6 }; for (int i = 0; i < 6; i++) a[n++] = &seg[0][order[i]]; }
    scenario("appended out of order (4, 5, 1, 2, 3, 6)", a, n, AV_CODEC_ID_H264, &r0, &r0, 1 << 30, r0.n, picture_scale, 3);
    n = 0; a[n++] = &init[2]; for (int i = 1; i <= 7; i++) a[n++] = &seg[2][i];
    scenario("AAC, segments 1-7", a, n, AV_CODEC_ID_AAC, &r2, &r2, 1 << 30, r2.n, 1.0, 3);

    for (int q = 0; q < 3; q++) {
        free(init[q].d);
        for (int i = 1; i <= (q == 2 ? 7 : 6); i++) free(seg[q][i].d);
    }
    free(r0.l); free(r1.l); free(r2.l);
    printf("media fragment decode: %lu checks, %lu failed\n", s_checks, s_failed);
    return s_failed != 0;
}
