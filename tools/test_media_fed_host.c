// test_media_fed_host — libmedia's fed media (os64/media_fed.h and media.c
// § THE FED MEDIA) on the host, driven by tools/test_media_fed_host.sh,
// which builds it under ThreadSanitizer and under ASan/UBSan over the
// pinned FFmpeg, with the fixture tools/make_dash_fixture.sh made
// (tools/pages/sync-fed-*.mp4 and their indexes) and ffmpeg's framecrc of
// each picture rendition.
//
//     test_media_fed_host <tools/pages> <directory of framecrc files>
//
//  1  STRAIGHT: the 720p pictures and the 44.1 kHz sound, each in a buffer
//     of its own, appended whole, then the end. Every picture comes out,
//     bit-exact against the host's ffmpeg (framecrc's Adler-32 of each
//     picture) and in presentation order; the sound runs to its length
//     with no silence and no cut (the placement plays it by count); every
//     slab's last frame ends at its last byte, which the decoders read
//     past into the fed media's padding.
//  2  A QUALITY SWITCH with B-frames: 720p's first two fragments, then
//     480p's initialization segment and its last three. Every picture of
//     both comes out (the decoder drained before its reopen, so none of
//     H.264's reordered pictures is lost), the first 240 at 1280x720 and
//     the rest at 854x480, each bit-exact against its own rendition; one
//     reopen.
//  3  A SEEK INTO WHAT IS NOT YET BUFFERED: the first two fragments, a seek
//     to 7 s, and GOT_WAIT; then the last two fragments, and the first
//     picture out is the one at 7 s, bit-exact.
//  4  TWO THREADS: one appends the fragments of both kinds a few
//     milliseconds apart while the other decodes, parked on GOT_WAIT; every
//     picture comes out, bit-exact. ThreadSanitizer must see no race.
//  5  TWO INITS IN ONE APPEND (seam change 10): the first one's duration.
//  6  A SWITCH APPENDED OVER WHAT IS BUFFERED AHEAD of the decoder plays on
//     to the end without waiting.
//  7  A RESYNC IN THE MIDDLE OF A GROUP drops pictures to the next keyframe,
//     which comes out next, and goes on to the end.
//  8  A REMOVAL PAST WHERE A DECODER FALLEN BEHIND IS READING: its cursors
//     resync on to the first data after the cut, and it plays to the end.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libavutil/adler32.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"

#include "media_fed_internal.h"
#include "media_picture.h"
#include "os64/media.h"
#include "os64/media_fed.h"

static unsigned s_checks, s_failed;
// Atomic: the feeding thread checks its appends too.
#define CHECK(ok, ...)                                    \
    do {                                                  \
        __atomic_fetch_add(&s_checks, 1, __ATOMIC_RELAXED); \
        if (!(ok)) {                                      \
            if (__atomic_add_fetch(&s_failed, 1, __ATOMIC_RELAXED) <= 40) { \
                printf("FAIL: ");                         \
                printf(__VA_ARGS__);                      \
                printf("\n");                             \
            }                                             \
        }                                                 \
    } while (0)

// ---------------------------------------------------------------- the fixture

typedef struct {
    uint8_t *data;
    size_t size;
    long init[2];
    int count;
    long frag[8][2];            // offset, length
    double start[8];
} rendition_t;

static void *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n + 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    d[n] = 0;
    *size = (size_t)n;
    return d;
}

// The index: "init":[o,l] ... "fragments":[[o,l,s,d],...] (the script's).
static void load(const char *dir, const char *name, rendition_t *r)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.mp4", dir, name);
    r->data = slurp(path, &r->size);
    snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    size_t n;
    char *j = slurp(path, &n);
    char *p = strstr(j, "\"init\"");
    if (!p || sscanf(strchr(p, '[') + 1, "%ld , %ld", &r->init[0], &r->init[1]) != 2) exit(2);
    p = strstr(j, "\"fragments\"");
    p = strchr(p, '[') + 1;
    r->count = 0;
    while ((p = strchr(p, '[')) != NULL && r->count < 8) {
        double d;
        if (sscanf(p + 1, "%ld , %ld , %lf , %lf", &r->frag[r->count][0], &r->frag[r->count][1],
                   &r->start[r->count], &d) != 4)
            break;
        r->count++;
        p++;
    }
    free(j);
}

// framecrc's lines: "0, pts, pts, duration, size, 0x<adler>".
static int load_crcs(const char *dir, const char *name, uint32_t *out, int cap)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.framecrc", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) { printf("cannot read %s\n", path); exit(2); }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < cap) {
        if (line[0] == '#') continue;
        char *x = strstr(line, "0x");
        if (x) out[n++] = (uint32_t)strtoul(x, NULL, 16);
    }
    fclose(f);
    return n;
}

static uint32_t picture_crc(const os64_media_picture_t *p)
{
    const AVFrame *f = media_picture_frame(p);
    int size = av_image_get_buffer_size(f->format, f->width, f->height, 1);
    uint8_t *buf = malloc((size_t)size);
    av_image_copy_to_buffer(buf, size, (const uint8_t *const *)f->data, f->linesize, f->format, f->width,
                            f->height, 1);
    uint32_t crc = av_adler32_update(0, buf, (size_t)size);
    free(buf);
    return crc;
}

static void append(os64_media_fed_t *fed, os64_media_fed_buffer_t *b, const rendition_t *r, long off, long len)
{
    CHECK(os64_media_fed_take(fed, b, r->data + off, (size_t)len), "take %ld bytes", len);
    os64_media_fed_result_t res;
    os64_media_fed_parse(fed, b, &res);
    CHECK(res.status == OS64_MEDIA_FED_DONE, "parse: %s", res.message);
}

static void append_frag(os64_media_fed_t *fed, os64_media_fed_buffer_t *b, const rendition_t *r, int i)
{
    append(fed, b, r, r->frag[i][0], r->frag[i][1]);
}

// ---------------------------------------------------------------- pulling

typedef struct {
    int pictures;
    uint32_t crc[700];
    int64_t at[700];
    uint32_t width[700];
    int64_t sound_frames, sound_first, sound_next;
    bool sound_gap;
    int waits;
} pulled_t;

// Pulls until GOT_END, or until `waits_allowed` GOT_WAITs in a row (each
// answered by `on_wait`, which may append; NULL: stop at the first).
static void pull(os64_media_t *m, pulled_t *out, int (*on_wait)(void *), void *ctx)
{
    int16_t pcm[2 * 4096];
    memset(out, 0, sizeof(*out));
    out->sound_first = -1;
    for (;;) {
        os64_media_got_t got;
        int r = os64_media_next(m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got);
        CHECK(r == 0, "next: %d (%s)", r, os64_media_error_text(m));
        if (r != 0) return;
        if (got.kind == OS64_MEDIA_GOT_END) return;
        if (got.kind == OS64_MEDIA_GOT_WAIT) {
            out->waits++;
            if (on_wait == NULL || !on_wait(ctx)) return;
            continue;
        }
        if (got.kind == OS64_MEDIA_GOT_PICTURE) {
            if (out->pictures < 700) {
                out->crc[out->pictures] = picture_crc(got.picture);
                out->at[out->pictures] = got.picture->at;
                out->width[out->pictures] = got.picture->width;
            }
            out->pictures++;
            os64_media_picture_release(m, got.picture);
        } else if (got.kind == OS64_MEDIA_GOT_SOUND) {
            if (out->sound_first < 0) out->sound_first = got.at;
            else if (got.at != out->sound_next) out->sound_gap = true;
            out->sound_next = got.at + got.frames;
            out->sound_frames += got.frames;
        }
    }
}

static os64_media_t *open_fed(os64_media_fed_t *fed)
{
    media_fed_lock(fed);
    bool picked = media_fed_picked(fed);
    media_fed_unlock(fed);
    CHECK(picked, "the tracks are picked once every buffer has an init");
    os64_media_t *m = NULL;
    os64_media_info_t info;
    int r = media_open_fed(fed, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, &m, &info);
    CHECK(r == 0 && m != NULL, "media_open_fed: %d", r);
    return m;
}

// ---------------------------------------------------------------- the cases

static rendition_t s_720, s_480, s_a44;
static uint32_t s_crc720[700], s_crc480[700];
static int s_n720, s_n480;

static void straight(void)
{
    os64_media_fed_t *fed;
    CHECK(os64_media_fed_create(&fed) == 0, "create");
    media_fed_hold(fed, NULL);      // media_open_fed's caller holds the fed media, as the player does
    os64_media_fed_buffer_t *v = os64_media_fed_add(fed, "video/mp4; codecs=\"avc1.640020\"");
    os64_media_fed_buffer_t *a = os64_media_fed_add(fed, "audio/mp4; codecs=\"mp4a.40.2\"");
    CHECK(v && a, "two buffers");
    append(fed, v, &s_720, s_720.init[0], s_720.init[1]);
    append(fed, a, &s_a44, s_a44.init[0], s_a44.init[1]);
    for (int i = 0; i < s_720.count; i++) append_frag(fed, v, &s_720, i);
    for (int i = 0; i < s_a44.count; i++) append_frag(fed, a, &s_a44, i);
    os64_media_fed_end(fed);
    os64_media_fed_range_t rv[4], ra[4];
    uint32_t nv = os64_media_fed_ranges(fed, v, false, rv, 4, NULL), na = os64_media_fed_ranges(fed, a, false, ra, 4, NULL);
    // The pictures start two frames late (B-frames, and no edit list in a
    // file made with an empty moov); the sound at 0.
    CHECK(nv == 1 && rv[0].start > 0 && rv[0].start < 0.04 && na == 1 && ra[0].start == 0,
          "straight: the buffers hold [%g, %g) (%u) and [%g, %g) (%u)", nv ? rv[0].start : -1, nv ? rv[0].end : -1, nv,
          na ? ra[0].start : -1, na ? ra[0].end : -1, na);
    os64_media_t *m = open_fed(fed);
    if (m == NULL) return;
    static pulled_t got;
    pull(m, &got, NULL, NULL);
    CHECK(got.pictures == s_n720, "straight: %d pictures, ffmpeg %d (%d waits)", got.pictures, s_n720, got.waits);
    int bad = 0;
    for (int i = 0; i < got.pictures && i < s_n720; i++) bad += got.crc[i] != s_crc720[i];
    CHECK(bad == 0, "straight: %d pictures differ from ffmpeg's", bad);
    for (int i = 1; i < got.pictures && i < 700; i++)
        if (got.at[i] <= got.at[i - 1]) { CHECK(0, "straight: picture %d not after %d", i, i - 1); break; }
    CHECK(!got.sound_gap && got.sound_first == 0, "straight: the sound is contiguous from 0");
    CHECK(got.sound_frames > 47 * 10000 && got.sound_frames < 49 * 10500, "straight: %lld frames of sound",
          (long long)got.sound_frames);
    os64_media_fed_counts_t c;
    os64_media_fed_counts(fed, &c);
    CHECK(c.silence_frames == 0 && c.cut_frames == 0 && c.resyncs == 0 && c.reopens == 0,
          "straight: silence %llu, cut %llu, resyncs %llu, reopens %llu", (unsigned long long)c.silence_frames,
          (unsigned long long)c.cut_frames, (unsigned long long)c.resyncs, (unsigned long long)c.reopens);
    os64_media_close(m);
    media_fed_release(fed);
    os64_media_fed_close(fed);
}

static void quality_switch(void)
{
    os64_media_fed_t *fed;
    os64_media_fed_create(&fed);
    media_fed_hold(fed, NULL);
    os64_media_fed_buffer_t *v = os64_media_fed_add(fed, "video/mp4; codecs=\"avc1.640020\"");
    append(fed, v, &s_720, s_720.init[0], s_720.init[1]);
    append_frag(fed, v, &s_720, 0);
    append_frag(fed, v, &s_720, 1);
    append(fed, v, &s_480, s_480.init[0], s_480.init[1]);
    for (int i = 2; i < s_480.count; i++) append_frag(fed, v, &s_480, i);
    os64_media_fed_end(fed);
    os64_media_t *m = open_fed(fed);
    if (m == NULL) return;
    static pulled_t got;
    pull(m, &got, NULL, NULL);
    CHECK(got.pictures == s_n720, "switch: %d pictures, %d expected (none lost at the reopen)", got.pictures, s_n720);
    int bad = 0, wrong_size = 0;
    for (int i = 0; i < got.pictures && i < 700; i++) {
        bool first = i < 240;
        bad += got.crc[i] != (first ? s_crc720[i] : s_crc480[i]);
        wrong_size += got.width[i] != (first ? 1280u : 854u);
    }
    CHECK(bad == 0 && wrong_size == 0, "switch: %d pictures differ, %d of the wrong size", bad, wrong_size);
    os64_media_fed_counts_t c;
    os64_media_fed_counts(fed, &c);
    CHECK(c.reopens == 1, "switch: %llu reopens", (unsigned long long)c.reopens);
    os64_media_close(m);
    media_fed_release(fed);
    os64_media_fed_close(fed);
}

typedef struct {
    os64_media_fed_t *fed;
    os64_media_fed_buffer_t *v, *a;
    bool appended;
} late_t;

static int append_rest(void *ctx)
{
    late_t *l = ctx;
    if (l->appended) return 0;
    l->appended = true;
    for (int i = 3; i < s_720.count; i++) append_frag(l->fed, l->v, &s_720, i);
    for (int i = 3; i < s_a44.count; i++) append_frag(l->fed, l->a, &s_a44, i);
    os64_media_fed_end(l->fed);
    return 1;
}

static void seek_unbuffered(void)
{
    late_t l = { 0 };
    os64_media_fed_create(&l.fed);
    media_fed_hold(l.fed, NULL);
    l.v = os64_media_fed_add(l.fed, "video/mp4; codecs=\"avc1.640020\"");
    l.a = os64_media_fed_add(l.fed, "audio/mp4; codecs=\"mp4a.40.2\"");
    append(l.fed, l.v, &s_720, s_720.init[0], s_720.init[1]);
    append(l.fed, l.a, &s_a44, s_a44.init[0], s_a44.init[1]);
    for (int i = 0; i < 2; i++) {
        append_frag(l.fed, l.v, &s_720, i);
        append_frag(l.fed, l.a, &s_a44, i);
    }
    os64_media_t *m = open_fed(l.fed);
    if (m == NULL) return;
    CHECK(os64_media_seek(m, 7 * 48000) == 0, "seek to 7 s");
    static pulled_t got;
    pull(m, &got, append_rest, &l);
    CHECK(got.waits >= 1, "seek: it waited for what was not buffered (%d)", got.waits);
    // The picture at 7 s is ffmpeg's 418th: the first is at 2/60 s.
    CHECK(got.pictures > 0 && got.at[0] == 7 * 48000 && got.crc[0] == s_crc720[418],
          "seek: the first picture is the one at 7 s (at %lld)", got.pictures ? (long long)got.at[0] : -1LL);
    CHECK(got.sound_first == 7 * 48000, "seek: the sound starts at 7 s (%lld)", (long long)got.sound_first);
    os64_media_close(m);
    media_fed_release(l.fed);
    os64_media_fed_close(l.fed);
}

typedef struct {
    os64_media_fed_t *fed;
    os64_media_fed_buffer_t *v, *a;
} feeder_t;

static void *feeder(void *arg)
{
    feeder_t *f = arg;
    for (int i = 1; i < s_720.count || i < s_a44.count; i++) {
        struct timespec t = { 0, 3000000 };
        nanosleep(&t, NULL);
        if (i < s_720.count) append_frag(f->fed, f->v, &s_720, i);
        if (i < s_a44.count) append_frag(f->fed, f->a, &s_a44, i);
    }
    os64_media_fed_end(f->fed);
    return NULL;
}

// A GOT_WAIT answered by a millisecond's sleep, for ten seconds at most:
// past that the case fails with what it had, rather than hanging.
static int park(void *ctx)
{
    int *waits = ctx;
    struct timespec t = { 0, 1000000 };
    nanosleep(&t, NULL);
    return ++*waits < 10000;
}

static void two_threads(void)
{
    feeder_t f;
    os64_media_fed_create(&f.fed);
    media_fed_hold(f.fed, NULL);
    f.v = os64_media_fed_add(f.fed, "video/mp4; codecs=\"avc1.640020\"");
    f.a = os64_media_fed_add(f.fed, "audio/mp4; codecs=\"mp4a.40.2\"");
    append(f.fed, f.v, &s_720, s_720.init[0], s_720.init[1]);
    append(f.fed, f.a, &s_a44, s_a44.init[0], s_a44.init[1]);
    append_frag(f.fed, f.v, &s_720, 0);
    append_frag(f.fed, f.a, &s_a44, 0);
    os64_media_t *m = open_fed(f.fed);
    if (m == NULL) return;
    pthread_t t;
    pthread_create(&t, NULL, feeder, &f);
    static pulled_t got;
    int waits = 0;
    pull(m, &got, park, &waits);
    pthread_join(t, NULL);
    CHECK(waits < 10000, "two threads: still waiting after ten seconds, %d pictures and %lld frames of sound out",
          got.pictures, (long long)got.sound_frames);
    int bad = 0;
    for (int i = 0; i < got.pictures && i < s_n720; i++) bad += got.crc[i] != s_crc720[i];
    CHECK(got.pictures == s_n720 && bad == 0, "two threads: %d pictures (%d), %d differ", got.pictures, s_n720, bad);
    os64_media_close(m);
    media_fed_release(f.fed);
    os64_media_fed_close(f.fed);
}

// Case 5: inits of 10 s and of 5 s (720p's, its mvhd duration written
// over) in one append; the parse completes both and answers the FIRST
// one's duration, as two appends apart would.
static void two_inits(void)
{
    long n = s_720.init[1];
    uint8_t *both = malloc((size_t)n * 2);
    memcpy(both, s_720.data, (size_t)n);
    memcpy(both + n, s_720.data, (size_t)n);
    uint8_t *mvhd = memmem(both, (size_t)n, "mvhd", 4);
    CHECK(mvhd != NULL && mvhd[4] == 0, "the init has a version-0 mvhd");
    if (mvhd == NULL) { free(both); return; }
    uint32_t scale = (uint32_t)mvhd[16] << 24 | (uint32_t)mvhd[17] << 16 | (uint32_t)mvhd[18] << 8 | mvhd[19];
    for (int k = 0; k < 2; k++) {
        uint8_t *d = mvhd + k * n + 20;
        uint32_t dur = scale * (k == 0 ? 10u : 5u);
        d[0] = (uint8_t)(dur >> 24); d[1] = (uint8_t)(dur >> 16); d[2] = (uint8_t)(dur >> 8); d[3] = (uint8_t)dur;
    }
    os64_media_fed_t *fed;
    os64_media_fed_create(&fed);
    os64_media_fed_buffer_t *v = os64_media_fed_add(fed, "video/mp4; codecs=\"avc1.640020\"");
    CHECK(os64_media_fed_take(fed, v, both, (size_t)n * 2), "take both");
    os64_media_fed_result_t res;
    os64_media_fed_parse(fed, v, &res);
    CHECK(res.status == OS64_MEDIA_FED_DONE && res.inits == 2, "two inits completed (%u)", res.inits);
    CHECK(res.init_duration == 10.0, "the first init's duration, 10 s (%g)", res.init_duration);
    os64_media_fed_close(fed);
    free(both);
}

// Case 6: a switch appended OVER fragments already buffered ahead of the
// decoder (gplay --fed's `w`, as a player switching quality refetches the
// fragments after the one playing): 720p's first four fragments, part of
// it decoded, then 480p's init and fragments 2 to 4 over them, and the
// end. It plays on to the end without waiting, its last picture near 10 s.
static void switch_over_buffered(void)
{
    os64_media_fed_t *fed;
    os64_media_fed_create(&fed);
    media_fed_hold(fed, NULL);
    os64_media_fed_buffer_t *v = os64_media_fed_add(fed, "video/mp4; codecs=\"avc1.640020\"");
    os64_media_fed_buffer_t *a = os64_media_fed_add(fed, "audio/mp4; codecs=\"mp4a.40.2\"");
    append(fed, v, &s_720, s_720.init[0], s_720.init[1]);
    append(fed, a, &s_a44, s_a44.init[0], s_a44.init[1]);
    for (int i = 0; i < 4; i++) append_frag(fed, v, &s_720, i);
    for (int i = 0; i < s_a44.count; i++) append_frag(fed, a, &s_a44, i);
    os64_media_t *m = open_fed(fed);
    if (m == NULL) return;
    int16_t pcm[2 * 4096];
    int pictures = 0, waits = 0;
    int64_t last_at = -1;
    bool ended = false, appended = false;
    while (!ended && waits < 3) {
        if (!appended && pictures >= 100) {
            append(fed, v, &s_480, s_480.init[0], s_480.init[1]);
            for (int i = 2; i < s_480.count; i++) append_frag(fed, v, &s_480, i);
            os64_media_fed_end(fed);
            appended = true;
        }
        os64_media_got_t got;
        int r = os64_media_next(m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got);
        CHECK(r == 0, "switch over: next %d", r);
        if (r != 0) break;
        if (got.kind == OS64_MEDIA_GOT_END) ended = true;
        else if (got.kind == OS64_MEDIA_GOT_WAIT) waits++;
        else if (got.kind == OS64_MEDIA_GOT_PICTURE) {
            pictures++;
            last_at = got.picture->at;
            os64_media_picture_release(m, got.picture);
        }
    }
    os64_media_fed_counts_t c;
    os64_media_fed_counts(fed, &c);
    CHECK(ended && waits == 0, "switch over: it played on to the end (ended %d, waits %d, %d pictures, last at %lld, "
          "resyncs %llu, reopens %llu)", ended, waits, pictures, (long long)last_at, (unsigned long long)c.resyncs,
          (unsigned long long)c.reopens);
    CHECK(last_at > 9 * 48000, "switch over: the last picture near the end (%lld)", (long long)last_at);
    os64_media_close(m);
    media_fed_release(fed);
    os64_media_fed_close(fed);
}

// Case 7: a resync (os64_media_picture_resync, a player far behind) in the
// middle of a group: pictures are dropped until the next keyframe, which
// is the next picture out, and they go on from there to the end.
static void resync_mid_group(void)
{
    os64_media_fed_t *fed;
    os64_media_fed_create(&fed);
    media_fed_hold(fed, NULL);
    os64_media_fed_buffer_t *v = os64_media_fed_add(fed, "video/mp4; codecs=\"avc1.640020\"");
    append(fed, v, &s_720, s_720.init[0], s_720.init[1]);
    for (int i = 0; i < s_720.count; i++) append_frag(fed, v, &s_720, i);
    os64_media_fed_end(fed);
    os64_media_t *m = open_fed(fed);
    if (m == NULL) return;
    int pictures = 0, after = 0;
    int64_t first_after = -1, last_at = -1;
    bool ended = false;
    while (!ended) {
        if (pictures == 30) CHECK(os64_media_picture_resync(m) == 0, "resync");
        os64_media_got_t got;
        int r = os64_media_next(m, OS64_MEDIA_WANT_PICTURE, NULL, 0, &got);
        CHECK(r == 0 && got.kind != OS64_MEDIA_GOT_WAIT, "resync: next %d, kind %d", r, got.kind);
        if (r != 0 || got.kind == OS64_MEDIA_GOT_WAIT) break;
        if (got.kind == OS64_MEDIA_GOT_END) ended = true;
        else if (got.kind == OS64_MEDIA_GOT_PICTURE) {
            if (pictures >= 30) {
                if (after++ == 0) {
                    first_after = got.picture->at;
                    CHECK(got.picture->keyframe, "resync: the first picture after it is a keyframe");
                }
            }
            pictures++;
            last_at = got.picture->at;
            os64_media_picture_release(m, got.picture);
        }
    }
    CHECK(first_after == 2 * 48000 + 1600, "resync: decoding starts again at the keyframe at 2 s (%lld)",
          (long long)first_after);
    CHECK(ended && last_at > 9 * 48000, "resync: it goes on to the end (last at %lld)", (long long)last_at);
    os64_media_close(m);
    media_fed_release(fed);
    os64_media_fed_close(fed);
}

// Case 8: a removal past where a decoder fallen behind is reading (a
// player far behind, the page removing what is behind ITS position):
// 30 pictures of the 720p and its sound decoded, then [0 s, 4 s) of both
// removed. Each cursor resyncs, on to the first data after the cut, and it
// plays on to the end without waiting.
static void remove_under_decoder(void)
{
    os64_media_fed_t *fed;
    os64_media_fed_create(&fed);
    media_fed_hold(fed, NULL);
    os64_media_fed_buffer_t *v = os64_media_fed_add(fed, "video/mp4; codecs=\"avc1.640020\"");
    os64_media_fed_buffer_t *a = os64_media_fed_add(fed, "audio/mp4; codecs=\"mp4a.40.2\"");
    append(fed, v, &s_720, s_720.init[0], s_720.init[1]);
    append(fed, a, &s_a44, s_a44.init[0], s_a44.init[1]);
    for (int i = 0; i < s_720.count; i++) append_frag(fed, v, &s_720, i);
    for (int i = 0; i < s_a44.count; i++) append_frag(fed, a, &s_a44, i);
    os64_media_fed_end(fed);
    os64_media_t *m = open_fed(fed);
    if (m == NULL) return;
    int16_t pcm[2 * 4096];
    int pictures = 0, waits = 0;
    int64_t last_at = -1;
    bool ended = false, removed = false;
    while (!ended && waits < 3) {
        if (!removed && pictures == 30) {
            os64_media_fed_result_t res;
            os64_media_fed_remove(fed, v, 0, 4.0, &res);
            os64_media_fed_remove(fed, a, 0, 4.0, &res);
            os64_media_fed_end(fed);            // a removal reopens an ended source
            removed = true;
        }
        os64_media_got_t got;
        int r = os64_media_next(m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got);
        CHECK(r == 0, "remove under: next %d", r);
        if (r != 0) break;
        if (got.kind == OS64_MEDIA_GOT_END) ended = true;
        else if (got.kind == OS64_MEDIA_GOT_WAIT) waits++;
        else if (got.kind == OS64_MEDIA_GOT_PICTURE) {
            pictures++;
            last_at = got.picture->at;
            os64_media_picture_release(m, got.picture);
        }
    }
    os64_media_fed_counts_t c;
    os64_media_fed_counts(fed, &c);
    CHECK(ended && waits == 0 && c.resyncs >= 1, "remove under: on to the end (ended %d, waits %d, resyncs %llu)",
          ended, waits, (unsigned long long)c.resyncs);
    CHECK(last_at > 9 * 48000, "remove under: the last picture near the end (%lld)", (long long)last_at);
    os64_media_close(m);
    media_fed_release(fed);
    os64_media_fed_close(fed);
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: test_media_fed_host <pages> <crcs>\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);       // each line out at once, whatever ends the run
    load(argv[1], "sync-fed-720p", &s_720);
    load(argv[1], "sync-fed-480p", &s_480);
    load(argv[1], "sync-fed-a44", &s_a44);
    s_n720 = load_crcs(argv[2], "sync-fed-720p", s_crc720, 700);
    s_n480 = load_crcs(argv[2], "sync-fed-480p", s_crc480, 700);
    CHECK(s_n720 == 600 && s_n480 == 600, "the references: %d and %d pictures", s_n720, s_n480);
    straight();
    quality_switch();
    seek_unbuffered();
    two_threads();
    two_inits();
    switch_over_buffered();
    resync_mid_group();
    remove_under_decoder();
    printf("media fed: %u checks, %u failed\n", s_checks, s_failed);
    return s_failed != 0;
}
