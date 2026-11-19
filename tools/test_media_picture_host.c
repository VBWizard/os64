// test_media_picture_host — libmedia's picture side on the host, driven by
// tools/test_media_picture_host.sh (which builds it under ThreadSanitizer
// and under ASan/UBSan, over the pinned FFmpeg built without threads).
//
//     test_media_picture_host <bbb-480p-3s-av.mp4> <directory of made media>
//
//  1  THE RELEASE RULE: one thread decodes (os64_media_next), a second
//     converts and releases, through a queue of eight, for 3,000 pictures
//     (the clip looped by seeks at its end), with a seek to another place
//     every 500 while pictures are still out. The release goes back to the
//     decoder only on the decoding thread (media.c § THREADS), so the
//     sanitizers must see no race and no misuse; at the end none is held.
//  2  THE TIMELINE (os64/media.h, THE START IS THE FILE'S), on files whose
//     sound or picture starts late, by 0.1 s and by 2 s: the zero is the
//     earlier start; the late sound comes after that much silence and the
//     late picture at that time; an open for the sound alone and one for
//     the picture alone agree with the open for both, picture for picture
//     and sample for sample; the length runs to the later end.
//  3  ROTATION: a picture the container says is turned 90 degrees
//     counterclockwise is to be turned 270 clockwise; the size stays the
//     stored one.
//  4  A KIND THAT CANNOT BE DECODED (os64/media.h, os64_media_open_for): a
//     picture in a codec not built plays its sound; a sound in a codec not
//     built (AC-3) plays its picture; each kind is named and said
//     unplayable; an open for that kind alone is UNSUPPORTED.
//  5  FALLING BEHIND: skipping the pictures nothing depends on gives fewer
//     pictures than all, but some; a resync after ten pictures gives a
//     keyframe next and counts what it dropped.
//  6  SEEKING: each of a spread of targets gives first the picture showing
//     at it (at <= target < at + duration) and the sound from it.
//  7  INTERLACED pictures (H.264 marks each frame) convert, field by field:
//     the scaler refuses a source and destination whose interlacing
//     differs, so the scaler's destination takes each picture's.
//  8  THE UNSCALED EDGE (media_scale.c): bbb's 854-wide pictures at their
//     own size and scaled to widths that are and are not multiples of
//     eight, with each scaler kind, into a buffer of exactly as many rows
//     of a pitch of the width, the width + 2 and the width + 10 pixels,
//     followed by a guard: every pixel of every row is written, and nothing
//     past the width or the buffer is. And an interlaced 854-wide clip with
//     the width kept and the height changed by a row, where a field is
//     unscaled though the frame is not. (ASan cannot see these writes,
//     which FFmpeg's assembly makes; the guard and the row tails are
//     checked by hand.)
// 10  A RESYNC IN THE LAST GROUP of pictures, with H.264's delayed pictures
//     still in the decoder: no picture comes after it, since the stream
//     ends before another keyframe (os64_media_picture_resync flushes the
//     decoder, which the end would otherwise drain).
// 12  ACCEPT (os64_media_next's mask), on a 20-second file whose pictures
//     pass the eight MiB of kept packets about 15 seconds in:
//     a. a consumer whose picture queue is full (sound only accepted) keeps
//        getting sound, contiguous, until the kept pictures reach the
//        bound, and is then told GOT_FULL naming the picture;
//     b. then the pictures come, in the file's order from the first, and
//        taking both to the end gives all 600 pictures and the whole sound,
//        each kind contiguous;
//     c. a seek with packets kept empties the lists, and lands as a seek
//        does (the picture showing at the target, the sound from it);
//     d. a resync with packets kept drops them up to the first keyframe
//        among them, which is the next picture out.
// 11  WHAT A SOUND-ONLY OPEN READS: no more of a 30-second MP4 than an
//     open for both does (FFmpeg's probe would otherwise read the sound for
//     five seconds waiting on the discarded picture; media.c holds its
//     analysis to a tenth of a second when a picture is discarded).
//  9  COVER ART is no picture: a sound with an attached picture opens for
//     both with the picture ABSENT, is refused NO_PICTURE for the picture
//     alone, and decodes the same sound, from the same zero, as the file
//     without it.

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "os64/media.h"

static int s_failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); s_failed++; } } while (0)

typedef struct {
    int fd;
    os64_media_t *m;
    os64_media_info_t info;
    int result;
} Clip;

static void clip_open(Clip *c, const char *path, uint32_t want)
{
    memset(c, 0, sizeof(*c));
    c->fd = open(path, O_RDONLY);
    if (c->fd < 0) { c->result = OS64_MEDIA_IO; return; }
    os64_media_source_t src;
    os64_media_file_source(c->fd, &src);
    c->result = os64_media_open_for(&src, want, &c->m, &c->info);
}

static void clip_close(Clip *c)
{
    if (c->m) os64_media_close(c->m);
    if (c->fd >= 0) close(c->fd);
    c->m = NULL;
    c->fd = -1;
}

// ------------------------------------------------------------ 1. the release rule

#define QUEUE 8
static struct {
    pthread_mutex_t lock;
    pthread_cond_t moved;
    os64_media_picture_t *ring[QUEUE];
    unsigned head, count;
    int ended;
    os64_media_t *m;
    long converted, refused;
} q = { .lock = PTHREAD_MUTEX_INITIALIZER, .moved = PTHREAD_COND_INITIALIZER };

static void *converter(void *arg)
{
    (void)arg;
    os64_media_scaler_t *s;
    static uint32_t pixels[160 * 90];
    if (os64_media_scaler_create(160, 90, 0, &s) != 0) return (void *)1;
    for (;;) {
        pthread_mutex_lock(&q.lock);
        while (q.count == 0 && !q.ended) pthread_cond_wait(&q.moved, &q.lock);
        if (q.count == 0) { pthread_mutex_unlock(&q.lock); break; }
        os64_media_picture_t *p = q.ring[q.head];
        q.head = (q.head + 1) % QUEUE;
        q.count--;
        pthread_cond_broadcast(&q.moved);
        pthread_mutex_unlock(&q.lock);
        if (os64_media_scaler_convert(s, p, pixels, 160 * 4) == 0) q.converted++;
        else q.refused++;
        os64_media_picture_release(q.m, p);
    }
    os64_media_scaler_destroy(s);
    return NULL;
}

static void test_release(const char *clip)
{
    Clip c;
    clip_open(&c, clip, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == 0, "release: open %d", c.result);
    if (c.result != 0) return;
    q.m = c.m;
    pthread_t t;
    pthread_create(&t, NULL, converter, NULL);
    static int16_t pcm[4096 * 2];
    long given = 0, seeks = 0;
    unsigned x = 12345;
    while (given < 3000) {
        os64_media_got_t got;
        int r = os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got);
        CHECK(r == 0, "release: next %d", r);
        if (r != 0) break;
        if (got.kind == OS64_MEDIA_GOT_END) { os64_media_seek(c.m, 0); continue; }
        if (got.kind != OS64_MEDIA_GOT_PICTURE) continue;
        pthread_mutex_lock(&q.lock);
        while (q.count == QUEUE) pthread_cond_wait(&q.moved, &q.lock);
        q.ring[(q.head + q.count) % QUEUE] = got.picture;
        q.count++;
        pthread_cond_broadcast(&q.moved);
        pthread_mutex_unlock(&q.lock);
        if (++given % 500 == 0) {
            // Elsewhere, with pictures from here still in the queue.
            x = x * 1103515245 + 12345;
            CHECK(os64_media_seek(c.m, (int64_t)(x % 140000)) == 0, "release: seek");
            seeks++;
        }
    }
    pthread_mutex_lock(&q.lock);
    q.ended = 1;
    pthread_cond_broadcast(&q.moved);
    pthread_mutex_unlock(&q.lock);
    pthread_join(t, NULL);
    os64_media_got_t got;
    os64_media_seek(c.m, 0);                 // a call on the decoding thread: the released go back
    os64_media_counts_t counts;
    os64_media_counts(c.m, &counts);
    CHECK(q.converted == given && q.refused == 0, "release: converted %ld of %ld", q.converted, given);
    CHECK(counts.pictures_held == 0, "release: %lu still held", (unsigned long)counts.pictures_held);
    (void)got;
    clip_close(&c);
    printf("  1 release: %ld pictures converted and released on a second thread, %ld seeks among them\n", given, seeks);
}

// ------------------------------------------------------------ 2. the timeline

typedef struct {
    long pictures;
    int64_t first_picture_at, picture_sum;     // the sum of every picture's time
    int64_t sound_frames, first_sound;         // the first non-silent output frame
    double sound_sum;                          // sum of the left samples
    int result;
} Pull;

static void pull(os64_media_t *m, bool has_sound, Pull *p)
{
    static int16_t pcm[4096 * 2];
    memset(p, 0, sizeof(*p));
    p->first_picture_at = p->first_sound = -1;
    for (;;) {
        os64_media_got_t got;
        int r = os64_media_next(m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, has_sound ? pcm : NULL, has_sound ? 4096 : 0, &got);
        if (r != 0) { p->result = r; return; }
        if (got.kind == OS64_MEDIA_GOT_END) return;
        if (got.kind == OS64_MEDIA_GOT_SOUND) {
            for (uint32_t i = 0; i < got.frames; i++) {
                if (p->first_sound < 0 && (pcm[2 * i] > 64 || pcm[2 * i] < -64)) p->first_sound = got.at + i;
                p->sound_sum += pcm[2 * i];
            }
            p->sound_frames += got.frames;
            continue;
        }
        if (p->first_picture_at < 0) p->first_picture_at = got.picture->at;
        p->picture_sum += got.picture->at;
        p->pictures++;
        os64_media_picture_release(m, got.picture);
    }
}

static void test_timeline(const char *dir)
{
    static const char *kFiles[] = { "sound_late_0.1", "sound_late_2", "picture_late_0.1", "picture_late_2" };
    for (int f = 0; f < 4; f++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.mp4", dir, kFiles[f]);
        double late = strstr(kFiles[f], "_2") ? 2.0 : 0.1;
        bool sound_late = kFiles[f][0] == 's';
        int64_t lead = (int64_t)(late * 48000);
        Clip both, sound, picture;
        Pull pb, ps, pp;
        clip_open(&both, path, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
        CHECK(both.result == 0, "%s: open %d", kFiles[f], both.result);
        if (both.result != 0) continue;
        int64_t frames = both.info.frames;
        pull(both.m, true, &pb);
        clip_close(&both);
        clip_open(&sound, path, OS64_MEDIA_WANT_SOUND);
        pull(sound.m, true, &ps);
        int64_t frames_s = sound.info.frames;
        clip_close(&sound);
        clip_open(&picture, path, OS64_MEDIA_WANT_PICTURE);
        pull(picture.m, false, &pp);
        int64_t frames_p = picture.info.frames;
        clip_close(&picture);
        // The late stream starts `lead` after the zero, within an AAC frame
        // (1,024) of priming either way, or a picture's period.
        if (sound_late) {
            CHECK(pb.first_picture_at == 0, "%s: first picture at %lld", kFiles[f], (long long)pb.first_picture_at);
            CHECK(pb.first_sound >= lead - 1100 && pb.first_sound <= lead + 1100, "%s: first sound at %lld, lead %lld",
                  kFiles[f], (long long)pb.first_sound, (long long)lead);
        } else {
            CHECK(pb.first_picture_at >= lead - 1600 && pb.first_picture_at <= lead + 1600,
                  "%s: first picture at %lld, lead %lld", kFiles[f], (long long)pb.first_picture_at, (long long)lead);
            CHECK(pb.first_sound >= 0 && pb.first_sound < 1100, "%s: first sound at %lld", kFiles[f],
                  (long long)pb.first_sound);
        }
        CHECK(pb.result == 0 && ps.result == 0 && pp.result == 0, "%s: pulls %d %d %d", kFiles[f], pb.result,
              ps.result, pp.result);
        CHECK(pb.pictures == 120 && pp.pictures == 120, "%s: pictures %ld %ld", kFiles[f], pb.pictures, pp.pictures);
        CHECK(pp.picture_sum == pb.picture_sum, "%s: the picture-only open's times differ", kFiles[f]);
        CHECK(ps.sound_frames == pb.sound_frames && ps.sound_sum == pb.sound_sum && ps.first_sound == pb.first_sound,
              "%s: the sound-only open's sound differs (%lld against %lld frames)", kFiles[f],
              (long long)ps.sound_frames, (long long)pb.sound_frames);
        CHECK(frames == frames_s && frames == frames_p, "%s: lengths %lld %lld %lld", kFiles[f], (long long)frames,
              (long long)frames_s, (long long)frames_p);
        int64_t end = (int64_t)((4.0 + late) * 48000);
        CHECK(frames >= end - 2000 && frames <= end + 2000, "%s: length %lld, about %lld", kFiles[f],
              (long long)frames, (long long)end);
        printf("  2 %s: first picture at %lld, first sound at %lld, length %lld\n", kFiles[f],
               (long long)pb.first_picture_at, (long long)pb.first_sound, (long long)frames);
    }
}

// ------------------------------------------------------------ 3, 4. the info

static void test_info(const char *dir)
{
    char path[512];
    Clip c;
    snprintf(path, sizeof(path), "%s/rotated_ccw90.mp4", dir);
    clip_open(&c, path, OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == 0 && c.info.rotation == 270 && c.info.coded_width == 96 && c.info.coded_height == 64 &&
          c.info.width == 96, "rotation: result %d rotation %u size %ux%u", c.result, c.info.rotation,
          c.info.coded_width, c.info.coded_height);
    uint32_t rotation = c.info.rotation;
    clip_close(&c);
    printf("  3 rotation: 90 counterclockwise in the container is %u clockwise\n", rotation);

    snprintf(path, sizeof(path), "%s/picture_mpeg4.mp4", dir);
    clip_open(&c, path, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == 0 && c.info.sound == OS64_MEDIA_PLAYABLE && c.info.picture == OS64_MEDIA_UNPLAYABLE &&
          strcmp(c.info.picture_codec, "mpeg4") == 0, "mpeg4 picture: result %d picture %u codec %s", c.result,
          c.info.picture, c.info.picture_codec);
    clip_close(&c);
    clip_open(&c, path, OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == OS64_MEDIA_UNSUPPORTED, "mpeg4 picture alone: %d", c.result);
    clip_close(&c);

    snprintf(path, sizeof(path), "%s/sound_ac3.mp4", dir);
    clip_open(&c, path, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == 0 && c.info.picture == OS64_MEDIA_PLAYABLE && c.info.sound == OS64_MEDIA_UNPLAYABLE &&
          strcmp(c.info.codec, "ac3") == 0 && c.info.rate == 0, "ac3 sound: result %d sound %u codec %s", c.result,
          c.info.sound, c.info.codec);
    if (c.result == 0) {
        Pull p;
        pull(c.m, false, &p);
        CHECK(p.result == 0 && p.pictures == 120 && p.sound_frames == 0, "ac3 sound: pictures %ld", p.pictures);
    }
    clip_close(&c);
    clip_open(&c, path, OS64_MEDIA_WANT_SOUND);
    CHECK(c.result == OS64_MEDIA_UNSUPPORTED, "ac3 sound alone: %d", c.result);
    clip_close(&c);
    printf("  4 a kind not built: named, unplayable, and the other kind plays\n");
}

// ------------------------------------------------------------ 5, 6. skipping and seeking

static void test_skip_and_seek(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/av.mp4", dir);
    Clip c;
    clip_open(&c, path, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == 0, "skip: open %d", c.result);
    if (c.result != 0) return;
    Pull p;
    CHECK(os64_media_picture_skip(c.m, OS64_MEDIA_SKIP_UNUSED) == 0, "skip");
    pull(c.m, true, &p);
    CHECK(p.result == 0 && p.pictures > 0 && p.pictures < 120, "skip unused: %ld pictures", p.pictures);
    long skipped = 120 - p.pictures;
    os64_media_picture_skip(c.m, OS64_MEDIA_SKIP_NONE);

    // A resync after ten pictures: the next is a keyframe.
    os64_media_seek(c.m, 0);
    static int16_t pcm[4096 * 2];
    long n = 0;
    bool key_next = false, resynced = false;
    for (;;) {
        os64_media_got_t got;
        if (os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got) != 0 || got.kind == OS64_MEDIA_GOT_END) break;
        if (got.kind != OS64_MEDIA_GOT_PICTURE) continue;
        if (resynced && !key_next) {
            key_next = true;
            CHECK(got.picture->keyframe && got.picture->at >= 30 * 1600, "resync: next picture at %lld key %d",
                  (long long)got.picture->at, got.picture->keyframe);
        }
        os64_media_picture_release(c.m, got.picture);
        if (++n == 10) { os64_media_picture_resync(c.m); resynced = true; }
    }
    os64_media_counts_t counts;
    os64_media_counts(c.m, &counts);
    CHECK(key_next && counts.pictures_resync > 0, "resync: dropped %lu", (unsigned long)counts.pictures_resync);

    // Seeks to a spread of targets.
    long seeks = 0;
    for (int64_t target = 0; target < 4 * 48000; target += 9973) {
        CHECK(os64_media_seek(c.m, target) == 0, "seek %lld", (long long)target);
        int64_t first_picture = -1, first_sound = -1;
        while (first_picture < 0 || first_sound < 0) {
            os64_media_got_t got;
            if (os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got) != 0 || got.kind == OS64_MEDIA_GOT_END) break;
            if (got.kind == OS64_MEDIA_GOT_SOUND && first_sound < 0) first_sound = got.at;
            if (got.kind == OS64_MEDIA_GOT_PICTURE) {
                if (first_picture < 0) {
                    first_picture = got.picture->at;
                    CHECK(got.picture->at <= target && target < got.picture->at + got.picture->duration,
                          "seek %lld: first picture at %lld", (long long)target, (long long)got.picture->at);
                }
                os64_media_picture_release(c.m, got.picture);
            }
        }
        CHECK(first_picture >= 0, "seek %lld: no picture came at all", (long long)target);
        CHECK(first_sound == target, "seek %lld: first sound at %lld", (long long)target, (long long)first_sound);
        seeks++;
    }
    clip_close(&c);
    printf("  5 skipping the unused left out %ld of 120; a resync dropped %lu\n", skipped,
           (unsigned long)counts.pictures_resync);
    printf("  6 %ld seeks, each landing on the picture showing at its target\n", seeks);
}

static void test_interlaced(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/interlaced.mp4", dir);
    Clip c;
    clip_open(&c, path, OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == 0, "interlaced: open %d", c.result);
    if (c.result != 0) return;
    os64_media_scaler_t *s;
    static uint32_t pixels[80 * 60];
    CHECK(os64_media_scaler_create(80, 60, 0, &s) == 0, "interlaced: scaler");
    long converted = 0, refused = 0;
    for (;;) {
        os64_media_got_t got;
        if (os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, NULL, 0, &got) != 0 || got.kind == OS64_MEDIA_GOT_END) break;
        if (os64_media_scaler_convert(s, got.picture, pixels, 80 * 4) == 0) converted++;
        else refused++;
        os64_media_picture_release(c.m, got.picture);
    }
    os64_media_scaler_destroy(s);
    clip_close(&c);
    // tinterlace weaves two pictures into one frame: 60 in the 4 s.
    CHECK(converted == 60 && refused == 0, "interlaced: converted %ld, refused %ld", converted, refused);
    printf("  7 interlaced: %ld pictures converted, %ld refused\n", converted, refused);
}

static void test_edge(const char *clip, const char *what, const uint32_t (*sizes)[2], int count)
{
    static const uint32_t kSlack[3] = { 0, 2, 10 };
    static const uint32_t kKinds[3] = { 0, OS64_MEDIA_SCALE_FAST, OS64_MEDIA_SCALE_EXACT };
    enum { GUARD = 64, BLANK = 0x11111111u };
    static uint32_t buffer[1289 * 719 + GUARD];
    long checked = 0;
    for (int z = 0; z < count; z++)
    for (int k = 0; k < 3; k++)
        for (int pi = 0; pi < 3; pi++) {
            const uint32_t W = sizes[z][0], H = sizes[z][1];
            Clip c;
            clip_open(&c, clip, OS64_MEDIA_WANT_PICTURE);
            os64_media_scaler_t *s;
            if (c.result != 0 || os64_media_scaler_create(W, H, kKinds[k], &s) != 0) {
                CHECK(0, "edge: open %d", c.result);
                clip_close(&c);
                return;
            }
            uint32_t pitch = W + kSlack[pi];
            for (int n = 0; n < 3; n++) {
                os64_media_got_t got;
                if (os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, NULL, 0, &got) != 0 || got.kind != OS64_MEDIA_GOT_PICTURE) break;
                for (size_t i = 0; i < (size_t)pitch * H + GUARD; i++) buffer[i] = BLANK;
                CHECK(os64_media_scaler_convert(s, got.picture, buffer, pitch * 4) == 0, "edge: convert");
                os64_media_picture_release(c.m, got.picture);
                long unwritten = 0, past_width = 0, past_end = 0;
                for (uint32_t y = 0; y < H; y++)
                    for (uint32_t x = 0; x < pitch; x++) {
                        bool blank = buffer[(size_t)y * pitch + x] == BLANK;
                        if (x < W && blank) unwritten++;
                        if (x >= W && !blank) past_width++;
                    }
                for (size_t i = (size_t)pitch * H; i < (size_t)pitch * H + GUARD; i++) past_end += buffer[i] != BLANK;
                CHECK(unwritten == 0 && past_width == 0 && past_end == 0,
                      "edge: %ux%u kind %u pitch %u: %ld unwritten, %ld past the width, %ld past the buffer", W, H,
                      kKinds[k], pitch, unwritten, past_width, past_end);
                checked++;
            }
            os64_media_scaler_destroy(s);
            clip_close(&c);
        }
    printf("  8 the unscaled edge, %s: %ld conversions, every pixel written and none past\n", what, checked);
}

static void test_cover(const char *dir)
{
    char with[512], without[512];
    snprintf(with, sizeof(with), "%s/sound_cover.m4a", dir);
    snprintf(without, sizeof(without), "%s/sound.m4a", dir);
    Clip c;
    clip_open(&c, with, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == 0 && c.info.picture == OS64_MEDIA_ABSENT && c.info.sound == OS64_MEDIA_PLAYABLE,
          "cover: open for both %d, picture %u", c.result, c.info.picture);
    clip_close(&c);
    clip_open(&c, with, OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == OS64_MEDIA_NO_PICTURE, "cover: open for the picture %d", c.result);
    clip_close(&c);
    Pull a, b;
    int64_t frames_a = 0, frames_b = 0;
    clip_open(&c, with, OS64_MEDIA_WANT_SOUND);
    if (c.result == 0) { pull(c.m, true, &a); frames_a = c.info.frames; }
    clip_close(&c);
    clip_open(&c, without, OS64_MEDIA_WANT_SOUND);
    if (c.result == 0) { pull(c.m, true, &b); frames_b = c.info.frames; }
    clip_close(&c);
    CHECK(a.sound_frames == b.sound_frames && a.sound_sum == b.sound_sum && a.first_sound == b.first_sound &&
          frames_a == frames_b, "cover: the sound differs with the cover (%lld against %lld frames)",
          (long long)a.sound_frames, (long long)b.sound_frames);
    printf("  9 cover art: no picture, and the same sound as without it\n");
}

static void test_resync_at_end(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/av.mp4", dir);
    Clip c;
    clip_open(&c, path, OS64_MEDIA_WANT_PICTURE);
    CHECK(c.result == 0, "resync at end: open %d", c.result);
    if (c.result != 0) return;
    // 3.5 s into the 4: the last keyframe is at 3.0 (a group of 30), so the
    // decoder is part way through the last group, holding B pictures.
    os64_media_seek(c.m, 3 * 48000 + 24000);
    long before = 0, after = 0;
    for (;;) {
        os64_media_got_t got;
        if (os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, NULL, 0, &got) != 0 || got.kind == OS64_MEDIA_GOT_END) break;
        os64_media_picture_release(c.m, got.picture);
        if (before < 0) after++;
        else if (++before == 2) {
            os64_media_picture_resync(c.m);
            before = -1;
        }
    }
    clip_close(&c);
    CHECK(before == -1 && after == 0, "resync at end: %ld pictures after the resync", after);
    printf(" 10 a resync in the last group: %ld pictures after it\n", after);
}

// What a run of os64_media_next with one `accept` gave.
typedef struct {
    long pictures, sound_frames, picture_gaps, sound_gaps;
    int64_t first_picture_at, last_picture_at, sound_next;
    bool first_key;
    int ended_by;                  // OS64_MEDIA_GOT_END, _FULL, or -1: the limit
    uint32_t full;
    int result;
} Run;

// Pulls with `accept` until the end, a FULL, or `pictures` pictures; the
// sound's continuity is judged against r->sound_next, carried across runs.
static void run_accept(os64_media_t *m, uint32_t accept, long pictures, Run *r)
{
    static int16_t pcm[4096 * 2];
    r->pictures = r->sound_frames = r->picture_gaps = r->sound_gaps = 0;
    r->first_picture_at = -1;
    r->first_key = false;
    r->ended_by = -1;
    r->result = 0;
    while (r->pictures < pictures) {
        os64_media_got_t got;
        int e = os64_media_next(m, accept, (accept & OS64_MEDIA_WANT_SOUND) ? pcm : NULL,
                                (accept & OS64_MEDIA_WANT_SOUND) ? 4096 : 0, &got);
        if (e != 0) { r->result = e; return; }
        if (got.kind == OS64_MEDIA_GOT_END || got.kind == OS64_MEDIA_GOT_FULL) {
            r->ended_by = got.kind;
            r->full = got.full;
            return;
        }
        if (got.kind == OS64_MEDIA_GOT_SOUND) {
            if (got.at != r->sound_next) r->sound_gaps++;
            r->sound_next = got.at + got.frames;
            r->sound_frames += got.frames;
            continue;
        }
        if (r->first_picture_at < 0) {
            r->first_picture_at = got.picture->at;
            r->first_key = got.picture->keyframe;
        } else if (got.picture->at != r->last_picture_at + 1600) {
            r->picture_gaps++;
        }
        r->last_picture_at = got.picture->at;
        r->pictures++;
        os64_media_picture_release(m, got.picture);
    }
}

static void test_accept(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/dense.mp4", dir);
    const uint32_t S = OS64_MEDIA_WANT_SOUND, P = OS64_MEDIA_WANT_PICTURE;
    Clip c;
    Run r = { 0 };
    os64_media_counts_t counts;

    // a and b.
    clip_open(&c, path, S | P);
    CHECK(c.result == 0, "accept: open %d", c.result);
    if (c.result != 0) return;
    run_accept(c.m, S, 1, &r);
    os64_media_counts(c.m, &counts);
    long sound_before_full = r.sound_frames;
    CHECK(r.result == 0 && r.ended_by == OS64_MEDIA_GOT_FULL && r.full == P && r.pictures == 0 && r.sound_gaps == 0 &&
          counts.picture_listed_bytes >= (8u << 20) && sound_before_full > 10 * 48000 &&
          sound_before_full < 20 * 48000, "accept a: ended by %d full %u, %ld sound frames, %lu bytes kept",
          r.ended_by, r.full, sound_before_full, (unsigned long)counts.picture_listed_bytes);
    run_accept(c.m, P, 60, &r);
    CHECK(r.result == 0 && r.pictures == 60 && r.first_picture_at == 0 && r.picture_gaps == 0 && r.sound_frames == 0,
          "accept b: %ld pictures from %lld, %ld gaps", r.pictures, (long long)r.first_picture_at, r.picture_gaps);
    long pictures = r.pictures, sound = sound_before_full;
    int64_t last_picture = r.last_picture_at;
    run_accept(c.m, S | P, 100000, &r);
    CHECK(r.result == 0 && r.ended_by == OS64_MEDIA_GOT_END && r.first_picture_at == last_picture + 1600 &&
          r.picture_gaps == 0 && r.sound_gaps == 0 && pictures + r.pictures == 600 &&
          sound + r.sound_frames >= 20 * 48000 - 2048 && sound + r.sound_frames <= 20 * 48000 + 2048,
          "accept b: %ld pictures in all, %ld sound frames", pictures + r.pictures, sound + r.sound_frames);
    clip_close(&c);

    // c: a seek with packets kept.
    clip_open(&c, path, S | P);
    r.sound_next = 0;
    run_accept(c.m, S, 1, &r);                 // to the bound
    os64_media_counts(c.m, &counts);
    bool kept = counts.picture_listed > 0;
    CHECK(os64_media_seek(c.m, 5 * 48000 + 800) == 0, "accept c: seek");
    os64_media_counts(c.m, &counts);
    CHECK(kept && counts.picture_listed == 0 && counts.sound_listed == 0, "accept c: lists after the seek %u %u",
          counts.picture_listed, counts.sound_listed);
    r.sound_next = 5 * 48000 + 800;
    run_accept(c.m, S | P, 1, &r);
    CHECK(r.result == 0 && r.first_picture_at == 5 * 48000 && r.sound_gaps == 0,
          "accept c: first picture at %lld after the seek", (long long)r.first_picture_at);
    clip_close(&c);

    // d: a resync with packets kept. Ten pictures taken, then sound alone for
    // a while, so the kept pictures start part way through a group.
    clip_open(&c, path, S | P);
    r.sound_next = 0;
    run_accept(c.m, S | P, 10, &r);
    run_accept(c.m, S, 1, &r);
    os64_media_counts(c.m, &counts);
    uint32_t before = counts.picture_listed;
    uint64_t dropped_before = counts.pictures_resync;
    CHECK(os64_media_picture_resync(c.m) == 0, "accept d: resync");
    os64_media_counts(c.m, &counts);
    run_accept(c.m, P, 1, &r);
    CHECK(r.result == 0 && r.pictures == 1 && r.first_key && r.first_picture_at % (30 * 1600) == 0 &&
          r.first_picture_at > 0 && counts.picture_listed < before && counts.pictures_resync > dropped_before,
          "accept d: next picture at %lld key %d; kept %u -> %u", (long long)r.first_picture_at, r.first_key, before,
          counts.picture_listed);
    clip_close(&c);
    printf(" 12 accept: %ld sound frames before the bound, then the pictures in order; a seek and a resync with "
           "packets kept\n", sound_before_full);
}

typedef struct {
    int fd;
    long bytes;
} Counted;

static int64_t counted_read(void *ctx, uint8_t *out, size_t cap)
{
    Counted *c = ctx;
    ssize_t n = read(c->fd, out, cap);
    if (n > 0) c->bytes += n;
    return n;
}

static int64_t counted_seek(void *ctx, uint64_t at)
{
    return lseek(((Counted *)ctx)->fd, (off_t)at, SEEK_SET);
}

// Bytes a file's open reads, for `want`.
static long open_reads(const char *path, uint32_t want)
{
    Counted c = { open(path, O_RDONLY), 0 };
    os64_media_source_t src = { &c, counted_read, counted_seek, lseek(c.fd, 0, SEEK_END), false };
    lseek(c.fd, 0, SEEK_SET);
    os64_media_t *m;
    int r = os64_media_open_for(&src, want, &m, NULL);
    if (r == 0) os64_media_close(m);
    close(c.fd);
    return r == 0 ? c.bytes : -1;
}

static void test_open_reads(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/long.mp4", dir);
    long sound = open_reads(path, OS64_MEDIA_WANT_SOUND);
    long both = open_reads(path, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    CHECK(sound > 0 && both > 0 && sound <= both, "open reads: sound only %ld bytes, both %ld", sound, both);
    printf(" 11 what an open reads: %ld bytes for the sound alone, %ld for both\n", sound, both);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: test_media_picture_host <bbb-480p-3s-av.mp4> <directory>\n");
        return 2;
    }
    test_release(argv[1]);
    test_timeline(argv[2]);
    test_info(argv[2]);
    test_skip_and_seek(argv[2]);
    test_interlaced(argv[2]);
    static const uint32_t kProgressive[][2] = { { 854, 480 }, { 853, 480 }, { 641, 361 }, { 1279, 719 }, { 640, 360 } };
    // Interlaced, the width kept and the height changed by a row: a field
    // is then unscaled although the frame is not.
    static const uint32_t kInterlaced[][2] = { { 854, 479 }, { 854, 481 }, { 854, 480 }, { 853, 479 } };
    test_edge(argv[1], "progressive at its own size and scaled", kProgressive, 5);
    char interlaced[512];
    snprintf(interlaced, sizeof(interlaced), "%s/interlaced_854.mp4", argv[2]);
    test_edge(interlaced, "interlaced, the height changed by a row", kInterlaced, 4);
    test_cover(argv[2]);
    test_resync_at_end(argv[2]);
    test_open_reads(argv[2]);
    test_accept(argv[2]);
    if (s_failed) {
        printf("FAIL %d checks\n", s_failed);
        return 1;
    }
    printf("PASS media picture host\n");
    return 0;
}
