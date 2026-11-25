// playertest — libmedia's player (os64/media_player.h) for a sound alone,
// as a page's <audio> runs it, with the tap listening. The media is a WAV
// made here in memory: half a second of a 1 kHz sine at 48 kHz stereo
// (24,000 frames, exactly 500 cycles, so a run joined to the next is the
// same sine with no seam).
//
//   1. it plays whole at a gain of one half: the tap hears 24,000 frames of
//      the file at half its level, the player ENDs at its length, and the
//      program has three threads while it plays (this one, the output's
//      audio thread, the decoding thread: a sound alone starts no picture
//      thread)
//   2. LOOP: it runs on past its end without a gap, the tap hearing the
//      file again and again sample for sample, the position folded into
//      its length and no end; turned off, it ends
//   3. WAIT: the device is held by this program, so the output is refused
//      it; the player is busy and its position stands still. Let go, the
//      device is the output's at its next try, and the file is heard whole
//      from its first frame
//   4. IDLE: paused, the decoding thread calls the source's idle hook while
//      it waits; libmedia's own reads never do
//   5. A PICTURE'S LOOP whose sound and pictures end apart: two clips in
//      /tests/pages, one with two seconds of pictures and one of sound
//      (loop-sound-short.mp4), one the other way (loop-picture-short.mp4),
//      each looped for five seconds while frames are asked for as a display
//      would: it keeps playing through every run, never buffering for half
//      a second, and the pace never asks a resync (made with ffmpeg's
//      testsrc and sine: 320x240 H.264 at 30 a second, AAC at 48 kHz);
//      and with a fifth of a second of sound (loop-sound-tiny.mp4: it must
//      not wait in buffering for half a second of sound that never
//      comes), and with 1.9 seconds (loop-sound-near.mp4: the next run is
//      queued before the sound drains, and its sound must wait out the
//      silence to the pictures' end, or the ear runs ahead and pictures
//      drop)
//   6. an MP3 of no stated length from a source of unknown size: the probe
//      cannot say, and the end does (as an element's duration does)
//   7. a seek asked while the player is still OPENING (its source's reads
//      held back) is carried out once it is open, under the generation it
//      answered: it plays from there
//   8. A PLANE (os64/media_player.h § A plane), with a window (skipped
//      without a desktop): the plane is made and the caller's `frame` is
//      refused; the first picture is published while hidden; placed and
//      wanted, pictures are published at the display's pace; `shown` gives
//      a picture whose pixels do not change while the pace moves on, and a
//      third view at once is BUSY; a new size past the plane's class goes
//      STALE while placed and READY after a hide, on a new plane; closed
//      with its plane still placed, the player finishes after the closing
//      bound and the kernel has taken the plane out; a plane refused (the
//      window's four slots taken) leaves `frame` the caller's
//   (2 also counts `look`: a sound alone playing on rings it for its
//   position)
//
// Exit codes: 0x91A50000 success, 0x91A500NN the step that failed.
// 0x91A500FE: skipped because something can be heard (sound_guard.h).

#include "../audiotest/sound_guard.h"
#include "os64/os64.h"
#include "os64/audio.h"
#include "os64/media_output.h"
#include "os64/media_player.h"
#include "os64/thread.h"
#include "os64/gui.h"
#include "os64/media_show.h"
#include "../avtest/sine_mp3.h"

#include <math.h>

#define PLAYERTEST_OK 0x91A50000
#define STEP(n)       (0x91A50000 | (n))
#define FRAMES        24000u
#define AMPLITUDE     12000.0
#define HEARD_MAX     (3u * 48000u)

static int die(int step, const char *why, int64_t got)
{
    os64_printf("playertest: FAIL step %d: %s (got %ld)\n", step, why, got);
    return (int)STEP(step);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint8_t s_wav[44 + FRAMES * 4];
static int16_t s_samples[FRAMES * 2];

static void make_wav(void)
{
    os64_memcpy(s_wav, "RIFF", 4); put32(s_wav + 4, 36 + FRAMES * 4); os64_memcpy(s_wav + 8, "WAVE", 4);
    os64_memcpy(s_wav + 12, "fmt ", 4); put32(s_wav + 16, 16);
    put16(s_wav + 20, 1); put16(s_wav + 22, 2); put32(s_wav + 24, 48000);
    put32(s_wav + 28, 48000 * 4); put16(s_wav + 32, 4); put16(s_wav + 34, 16);
    os64_memcpy(s_wav + 36, "data", 4); put32(s_wav + 40, FRAMES * 4);
    for (uint32_t i = 0; i < FRAMES; i++) {
        int16_t v = (int16_t)lrint(AMPLITUDE * sin(2.0 * 3.14159265358979323846 * 1000.0 * (double)i / 48000.0));
        s_samples[i * 2] = v;
        s_samples[i * 2 + 1] = (int16_t)-v;
        put16(s_wav + 44 + i * 4, (uint16_t)v);
        put16(s_wav + 46 + i * 4, (uint16_t)(int16_t)-v);
    }
}

// The source: the WAV in memory, its idle calls counted.
typedef struct {
    uint64_t at;
    volatile uint32_t idles;
    volatile bool gated;            // reads wait until it opens (step 7: a seek while OPENING)
} memory_t;

static int64_t memory_read(void *ctx, uint8_t *out, size_t cap)
{
    memory_t *m = ctx;
    while (__atomic_load_n(&m->gated, __ATOMIC_ACQUIRE))
        os64_sleep(1);
    size_t left = sizeof(s_wav) - m->at, n = cap < left ? cap : left;
    os64_memcpy(out, s_wav + m->at, n);
    m->at += n;
    return (int64_t)n;
}

static int64_t memory_seek(void *ctx, uint64_t offset)
{
    memory_t *m = ctx;
    if (offset > sizeof(s_wav))
        return -1;
    m->at = offset;
    return (int64_t)offset;
}

static void memory_idle(void *ctx)
{
    memory_t *m = ctx;
    __atomic_add_fetch(&m->idles, 1, __ATOMIC_RELAXED);
}

// The player's `look`: counted, so a sound alone's steady play can be seen
// to ring it (for a page's timeupdate).
static volatile uint32_t s_looks;
static void counted_look(void *ctx) { (void)ctx; __atomic_add_fetch(&s_looks, 1, __ATOMIC_RELAXED); }

static volatile bool s_finished;
static void finished(void *ctx) { (void)ctx; __atomic_store_n(&s_finished, true, __ATOMIC_RELEASE); }

// The tap: what the device was given, the silent records left out.
static int32_t s_tap = -1;
static int16_t s_heard[HEARD_MAX * 2];
static uint32_t s_heard_n;
static bool s_dropped;

static void listen(uint64_t ms)
{
    static uint8_t buf[8 * OS64_AUDIO_TAP_READ_MIN];
    int64_t n = os64_read_for(s_tap, buf, sizeof(buf), ms);
    for (size_t at = 0; n > 0 && at < (size_t)n;) {
        os64_audio_tap_t h;
        os64_memcpy(&h, buf + at, sizeof(h));
        at += sizeof(h);
        if (h.dropped != 0)
            s_dropped = true;
        if (!h.silence && s_heard_n + h.frames <= HEARD_MAX) {
            os64_memcpy(s_heard + s_heard_n * 2, buf + at, h.frames * 4);
            s_heard_n += h.frames;
        }
        at += h.frames * 4;
    }
}

// Listens for `ms`, reading the tap as it goes.
static void listen_for(uint64_t ms)
{
    uint64_t end = (uint64_t)os64_micros() + ms * 1000;
    while ((uint64_t)os64_micros() < end)
        listen(20);
}

// Where the file starts in what was heard: the sine's first sample is 0, so
// the first sample heard that is not is the file's second.
static int64_t heard_start(void)
{
    for (uint32_t i = 0; i < s_heard_n; i++)
        if (s_heard[i * 2] != 0)
            return i > 0 ? (int64_t)i - 1 : -1;
    return -1;
}

static int threads_now(void)
{
    static char text[1024];
    int64_t h = os64_open("/proc/self/status", "r");
    int64_t n = h < 0 ? h : os64_read((int32_t)h, text, sizeof(text) - 1);
    if (h >= 0)
        os64_close((int32_t)h);
    if (n <= 0)
        return -1;
    text[n] = 0;
    for (char *line = text; line != NULL && *line;) {
        char *next = os64_strchr(line, '\n');
        if (next != NULL)
            *next++ = 0;
        if (os64_memcmp(line, "threads\t", 8) == 0)
            return (int)os64_atoi(line + 8);
        line = next;
    }
    return -1;
}

typedef struct {
    os64_media_output_t *output;
    os64_media_player_t *player;
    memory_t source;
} rig_t;

static bool rig_open_gated(rig_t *r, bool gated)
{
    os64_memset(r, 0, sizeof(*r));
    r->source.gated = gated;
    s_finished = false;
    if (os64_media_output_create(&r->output) != 0)
        return false;
    os64_media_player_config_t cfg = {
        .source = {&r->source, memory_read, memory_seek, (int64_t)sizeof(s_wav), false, memory_idle},
        .want = OS64_MEDIA_WANT_SOUND,
        .output = r->output,
        .refused = OS64_MEDIA_VOICE_WAIT,
        .loaded = true,
        .look = counted_look,
        .finished = finished,
        .ctx = r};
    return os64_media_player_create(&cfg, &r->player) == 0;
}

static bool rig_open(rig_t *r)
{
    return rig_open_gated(r, false);
}

static void rig_close(rig_t *r)
{
    if (r->player != NULL)
        os64_media_player_close(r->player);
    for (int i = 0; i < 500 && r->player != NULL && !__atomic_load_n(&s_finished, __ATOMIC_ACQUIRE); i++)
        os64_sleep(10);
    // The output's voice is let go after `finished`; destroy refuses until it is gone.
    for (int i = 0; i < 100 && r->output != NULL && os64_media_output_destroy(r->output) != 0; i++)
        os64_sleep(10);
}

static os64_media_player_facts_t facts(rig_t *r)
{
    os64_media_player_facts_t f;
    os64_media_player_facts(r->player, &f);
    return f;
}

// Waits up to `ms` for the player's state, listening meanwhile.
static bool wait_state(rig_t *r, int state, uint64_t ms)
{
    uint64_t end = (uint64_t)os64_micros() + ms * 1000;
    while ((uint64_t)os64_micros() < end) {
        if (facts(r).state == state)
            return true;
        listen(10);
    }
    return facts(r).state == state;
}

static int step_whole(void)
{
    rig_t r;
    if (!rig_open(&r))
        return die(1, "the output or the player would not start", 0);
    if (!wait_state(&r, OS64_MEDIA_PLAYER_PAUSED, 3000))
        return die(1, "the player did not open the WAV", facts(&r).state);
    os64_media_player_gain(r.player, 0.5);
    s_heard_n = 0;
    s_dropped = false;
    os64_media_player_play(r.player);
    if (!wait_state(&r, OS64_MEDIA_PLAYER_PLAYING, 3000))
        return die(1, "it did not start playing", facts(&r).state);
    int threads = threads_now();
    if (!wait_state(&r, OS64_MEDIA_PLAYER_ENDED, 5000))
        return die(1, "it did not end", facts(&r).state);
    listen_for(300);
    os64_media_player_facts_t f = facts(&r);
    rig_close(&r);
    os64_printf("playertest: %d threads while it played; heard %u frames\n", threads, s_heard_n);
    if (threads != 3)
        return die(1, "a sound alone ran with other than three threads", threads);
    if (f.position != FRAMES || f.length != FRAMES)
        return die(1, "it did not end at its length", f.position);
    if (s_dropped)
        return die(1, "the tap dropped records: held off too long to judge", 0);
    int64_t start = heard_start();
    if (start < 0 || s_heard_n - (uint32_t)start < FRAMES)
        return die(1, "the tap did not hear the file whole", s_heard_n);
    for (uint32_t i = 0; i < FRAMES; i++) {
        int32_t want = s_samples[i * 2] / 2, got = s_heard[(start + i) * 2];
        if (got < want - 1 || got > want + 1)
            return die(1, "the gain of one half was not applied (the frame)", i);
    }
    return 0;
}

static int step_loop(void)
{
    rig_t r;
    if (!rig_open(&r))
        return die(2, "the output or the player would not start", 0);
    if (!wait_state(&r, OS64_MEDIA_PLAYER_PAUSED, 3000))
        return die(2, "the player did not open the WAV", facts(&r).state);
    os64_media_player_loop(r.player, true);
    s_heard_n = 0;
    s_dropped = false;
    os64_media_player_play(r.player);
    // Two and a half runs and more, the looks counted from a second in
    // (past buffering's moves): a quarter second apart, so six or so.
    uint64_t end = (uint64_t)os64_micros() + 1600000;
    uint64_t counted_from = (uint64_t)os64_micros() + 400000;
    uint32_t looks_at = 0;
    bool counting = false;
    while ((uint64_t)os64_micros() < end) {
        os64_media_player_facts_t f = facts(&r);
        if (f.state == OS64_MEDIA_PLAYER_ENDED)
            return die(2, "a looping sound ended", f.position);
        if (f.position < 0 || f.position > FRAMES)
            return die(2, "the position was not folded into the length", f.position);
        if (!counting && (uint64_t)os64_micros() >= counted_from) {
            counting = true;
            looks_at = __atomic_load_n(&s_looks, __ATOMIC_RELAXED);
        }
        listen(10);
    }
    uint32_t looks = __atomic_load_n(&s_looks, __ATOMIC_RELAXED) - looks_at;
    if (s_dropped)
        return die(2, "the tap dropped records: held off too long to judge", 0);
    int64_t start = heard_start();
    uint32_t heard = start < 0 ? 0 : s_heard_n - (uint32_t)start;
    os64_printf("playertest: looping, heard %u frames (%u runs and more), %u looks in 1.2 s\n", heard, heard / FRAMES,
                looks);
    if (looks < 3)
        return die(2, "a sound alone playing on did not ring look for its position", looks);
    if (heard < 2 * FRAMES + FRAMES / 2)
        return die(2, "it did not run on past its end", heard);
    for (uint32_t i = 0; i < heard; i++)
        if (s_heard[(start + i) * 2] != s_samples[(i % FRAMES) * 2] ||
            s_heard[(start + i) * 2 + 1] != s_samples[(i % FRAMES) * 2 + 1])
            return die(2, "a run did not follow the last sample for sample (the frame)", i);
    os64_media_player_loop(r.player, false);
    // The runs already queued (the queue's two seconds) play out first.
    if (!wait_state(&r, OS64_MEDIA_PLAYER_ENDED, 5000))
        return die(2, "turned off, it did not end", facts(&r).state);
    os64_media_player_facts_t f = facts(&r);
    rig_close(&r);
    if (f.position != FRAMES)
        return die(2, "it did not end at its length", f.position);
    return 0;
}

static int step_wait(void)
{
    int64_t mine = os64_open("/dev/audio", "w");
    if (mine < 0)
        return die(3, "the device would not open for this test to hold", mine);
    rig_t r;
    if (!rig_open(&r)) {
        os64_close((int32_t)mine);
        return die(3, "the output or the player would not start", 0);
    }
    if (!wait_state(&r, OS64_MEDIA_PLAYER_PAUSED, 3000)) {
        os64_close((int32_t)mine);
        return die(3, "the player did not open the WAV", facts(&r).state);
    }
    s_heard_n = 0;
    s_dropped = false;
    os64_media_player_play(r.player);
    listen_for(800);
    os64_media_player_facts_t held = facts(&r);
    listen_for(400);
    os64_media_player_facts_t later = facts(&r);
    os64_close((int32_t)mine);
    if (!later.busy)
        return die(3, "with the device held elsewhere, the player was not busy", later.state);
    if (held.position != later.position || later.position > 4096)
        return die(3, "its position moved while it waited", later.position);
    if (s_heard_n != 0)
        return die(3, "something was heard while the device was held", s_heard_n);
    // The output tries again every OS64_MEDIA_OUTPUT_RETRY_MS.
    if (!wait_state(&r, OS64_MEDIA_PLAYER_ENDED, 5000))
        return die(3, "let go, the device did not play it to its end", facts(&r).state);
    listen_for(300);
    os64_media_player_facts_t f = facts(&r);
    rig_close(&r);
    if (s_dropped)
        return die(3, "the tap dropped records: held off too long to judge", 0);
    int64_t start = heard_start();
    os64_printf("playertest: waited at frame %ld, then heard %u frames\n", later.position, s_heard_n);
    if (start < 0 || s_heard_n - (uint32_t)start < FRAMES)
        return die(3, "the file was not heard whole once the device came back", s_heard_n);
    for (uint32_t i = 0; i < FRAMES; i++)
        if (s_heard[(start + i) * 2] != s_samples[i * 2])
            return die(3, "what was heard is not the file from its first frame (the frame)", i);
    if (f.position != FRAMES)
        return die(3, "it did not end at its length", f.position);
    return 0;
}

static int step_idle(void)
{
    rig_t r;
    if (!rig_open(&r))
        return die(4, "the output or the player would not start", 0);
    if (!wait_state(&r, OS64_MEDIA_PLAYER_PAUSED, 3000))
        return die(4, "the player did not open the WAV", facts(&r).state);
    uint32_t before = __atomic_load_n(&r.source.idles, __ATOMIC_RELAXED);
    os64_sleep(1000);
    uint32_t after = __atomic_load_n(&r.source.idles, __ATOMIC_RELAXED);
    rig_close(&r);
    // At least one a wake, and a parked thread wakes 200 ms apart at most.
    os64_printf("playertest: paused, %u idle calls in a second\n", after - before);
    if (after - before < 3)
        return die(4, "paused, the source's idle hook was not called as it waited", after - before);
    return 0;
}

typedef struct {
    int32_t file;
} clip_t;

static int64_t clip_read(void *ctx, uint8_t *out, size_t cap)
{
    clip_t *c = ctx;
    return os64_read(c->file, out, cap);
}

static int64_t clip_seek(void *ctx, uint64_t offset)
{
    clip_t *c = ctx;
    return os64_seek(c->file, (int64_t)offset, OS64_SEEK_SET);
}

static int step_picture_loop(const char *path)
{
    clip_t clip = {(int32_t)os64_open(path, "r")};
    if (clip.file < 0)
        return die(5, "the clip would not open", clip.file);
    int64_t size = os64_seek(clip.file, 0, OS64_SEEK_END);
    os64_seek(clip.file, 0, OS64_SEEK_SET);
    rig_t r;
    os64_memset(&r, 0, sizeof(r));
    s_finished = false;
    if (os64_media_output_create(&r.output) != 0)
        return die(5, "the output would not start", 0);
    os64_media_player_config_t cfg = {
        .source = {&clip, clip_read, clip_seek, size, false, NULL},
        .want = OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE,
        .output = r.output,
        .refused = OS64_MEDIA_VOICE_PLAY_ON,
        .loaded = true,
        .look = counted_look,
        .finished = finished,
        .ctx = &r};
    if (os64_media_player_create(&cfg, &r.player) != 0)
        return die(5, "the player would not start", 0);
    os64_media_player_size(r.player, 320, 240);
    os64_media_player_loop(r.player, true);
    os64_media_player_play(r.player);
    const int64_t period = 16667;
    int64_t start = os64_micros(), buffering_since = -1, longest = 0, last = -1;
    int wraps = 0, code = 0;
    bool played = false;
    while (os64_micros() - start < 5000000) {
        os64_media_player_frame_t fr;
        (void)os64_media_player_frame(r.player, os64_micros() + period, period, &fr);
        os64_media_player_facts_t f = facts(&r);
        int64_t now = os64_micros();
        if (f.state == OS64_MEDIA_PLAYER_PLAYING)
            played = true;
        if (f.state == OS64_MEDIA_PLAYER_ENDED || f.state == OS64_MEDIA_PLAYER_FAILED) {
            code = die(5, "a looping clip ended or failed (the state)", f.state);
            break;
        }
        if (played && f.state == OS64_MEDIA_PLAYER_BUFFERING) {
            if (buffering_since < 0)
                buffering_since = now;
            if (now - buffering_since > longest)
                longest = now - buffering_since;
        } else {
            buffering_since = -1;
        }
        if (last >= 0 && f.position < last - 24000)
            wraps++;
        last = f.position;
        os64_sleep(16);
    }
    os64_media_player_facts_t f = facts(&r);
    rig_close(&r);
    os64_close(clip.file);
    if (code != 0)
        return code;
    os64_printf("playertest: %s looped %d times; longest buffering %ld ms, resyncs %lu, dropped %lu\n", path, wraps,
                longest / 1000, (unsigned long)f.pace.resyncs, (unsigned long)f.pace.dropped);
    if (!played || longest > 500000)
        return die(5, "a looping clip stuck buffering (ms)", longest / 1000);
    if (wraps < 2)
        return die(5, "a looping clip did not run on past its end (runs)", wraps);
    if (f.pace.resyncs != 0)
        return die(5, "the pace asked a resync as a run's pictures ended", (int64_t)f.pace.resyncs);
    if (f.pace.dropped > 3)
        return die(5, "a looping clip dropped pictures (the ear ran ahead?)", (int64_t)f.pace.dropped);
    return 0;
}

// /sys/gui's count of planes in the machine.
static int64_t gui_planes(void)
{
    static char text[32768];
    int64_t h = os64_open("/sys/gui", "r");
    if (h < 0)
        return -1;
    int64_t n = 0, got;
    while (n < (int64_t)sizeof(text) - 1 && (got = os64_read((int32_t)h, text + n, sizeof(text) - 1 - (size_t)n)) > 0)
        n += got;
    os64_close((int32_t)h);
    text[n] = 0;
    for (int64_t i = 0; i + 8 <= n; i++)
        if (os64_memcmp(text + i, "planes: ", 8) == 0)
            return os64_atoi(text + i + 8);
    return -1;
}

static uint64_t view_sum(const os64_media_player_view_t *v)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < v->w * 8 && i < v->w * v->h; i++)
        sum = sum * 31 + v->pixels[i];
    return sum;
}

typedef struct {
    rig_t r;
    clip_t clip;
} plane_rig_t;

static bool plane_rig_open(plane_rig_t *pr)
{
    os64_memset(pr, 0, sizeof(*pr));
    pr->clip.file = (int32_t)os64_open("/tests/pages/loop-sound-short.mp4", "r");
    if (pr->clip.file < 0)
        return false;
    int64_t size = os64_seek(pr->clip.file, 0, OS64_SEEK_END);
    os64_seek(pr->clip.file, 0, OS64_SEEK_SET);
    s_finished = false;
    if (os64_media_output_create(&pr->r.output) != 0)
        return false;
    os64_media_player_config_t cfg = {
        .source = {&pr->clip, clip_read, clip_seek, size, false, NULL},
        .want = OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE,
        .output = pr->r.output,
        .refused = OS64_MEDIA_VOICE_PLAY_ON,
        .loaded = true,
        .look = counted_look,
        .finished = finished,
        .ctx = &pr->r};
    if (os64_media_player_create(&cfg, &pr->r.player) != 0)
        return false;
    os64_media_player_size(pr->r.player, 320, 240);
    os64_media_player_loop(pr->r.player, true);
    return true;
}

static void plane_rig_close(plane_rig_t *pr)
{
    rig_close(&pr->r);
    os64_close(pr->clip.file);
}

// Waits up to `ms` for a fact the test names.
#define WAIT_FACT(pr, ms, cond)                                                           ({                                                                                        os64_media_player_facts_t f_;                                                         int64_t until_ = os64_micros() + (int64_t)(ms) * 1000;                                do {                                                                                      f_ = facts(&(pr)->r);                                                                 if (cond)                                                                                 break;                                                                            os64_sleep(10);                                                                   } while (os64_micros() < until_);                                                     f_;                                                                               })

static int64_t place(int64_t win, uint32_t plane, os64_gui_rect_t origin, os64_gui_rect_t clip)
{
    os64_gui_plane_edit_t e = {0};
    e.plane = plane;
    e.origin = origin;
    e.clip = clip;
    return os64_gui_window_publish_edits(win, NULL, &e, 1);
}

static int64_t hide(int64_t win, uint32_t plane)
{
    os64_gui_plane_edit_t e = {.plane = plane, .hide = 1};
    return os64_gui_window_publish_edits(win, NULL, &e, 1);
}

static int step_plane(void)
{
    int64_t win = os64_gui_window_create("playertest", 80, 80, 400, 300, OS64_GUI_CREATE_CONTENT_SIZE);
    if (win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("playertest: step 8 skipped: no desktop\n");
        return 0;
    }
    if (win <= 0)
        return die(8, "the window would not open", win);
    os64_gui_surface_t surf;
    if (os64_gui_window_get_surface(win, &surf) == 0) {
        for (uint32_t y = 0; y < surf.height; y++)
            for (uint32_t x = 0; x < surf.width; x++)
                surf.pixels[(size_t)y * surf.pitch_px + x] =
                    x >= 40 && x < 360 && y >= 30 && y < 270 ? 0 : 0xFF202830u;   // a see-through hole
        os64_gui_window_publish(win, NULL);
    }
    int64_t planes_before = gui_planes();
    plane_rig_t pr;
    if (!plane_rig_open(&pr))
        return die(8, "the clip or its player would not open", 0);
    int code = 0;
    os64_media_player_frame_t fr;
    int rc = os64_media_player_plane(pr.r.player, win);
    os64_media_player_facts_t f = facts(&pr.r);
    if (rc != 0 || f.plane_state != OS64_MEDIA_PLANE_ON || f.plane_name == 0) {
        code = die(8, "the plane was not made (the call)", rc);
        goto out;
    }
    if (os64_media_player_frame(pr.r.player, os64_micros(), 16667, &fr) || facts(&pr.r).frame_refused == 0) {
        code = die(8, "the caller's frame was not refused with a plane on", 0);
        goto out;
    }
    os64_media_player_play(pr.r.player);
    f = WAIT_FACT(&pr, 3000, f_.plane_published > 0 && f_.plane_ready);
    if (f.plane_published == 0 || f.plane_pic_w != 320 || f.plane_pic_h != 240) {
        code = die(8, "the first picture was not published while hidden (published)", (int64_t)f.plane_published);
        goto out;
    }
    os64_gui_rect_t hole = {40, 30, 320, 240};
    if (place(win, f.plane_name, hole, hole) != 0) {
        code = die(8, "the plane would not be placed", place(win, f.plane_name, hole, hole));
        goto out;
    }
    os64_media_player_plane_want(pr.r.player, true);
    uint64_t published = f.plane_published;
    os64_sleep(1000);
    f = facts(&pr.r);
    os64_printf("playertest: plane %u: %lu published in a second placed, %lu display frames missed\n", f.plane_name,
                (unsigned long)(f.plane_published - published), (unsigned long)f.frames_missed);
    if (f.plane_published < published + 10) {
        code = die(8, "placed and playing, pictures were not published at the display's pace", (int64_t)(f.plane_published - published));
        goto out;
    }

    // `shown`: pinned pixels while the pace moves on; two views at most.
    os64_media_player_view_t a, b, c;
    if (os64_media_player_shown(pr.r.player, &a) != 0) {
        code = die(8, "shown gave nothing while playing", 0);
        goto out;
    }
    uint64_t sum = view_sum(&a);
    os64_sleep(300);
    rc = os64_media_player_shown(pr.r.player, &b);
    int third = rc == 0 ? os64_media_player_shown(pr.r.player, &c) : 0;
    bool kept = view_sum(&a) == sum, moved = rc == 0 && b.serial != a.serial;
    if (third == 0)
        os64_media_player_shown_done(pr.r.player, &c);
    if (rc == 0)
        os64_media_player_shown_done(pr.r.player, &b);
    os64_media_player_shown_done(pr.r.player, &a);
    if (!kept || !moved || third != OS64_MEDIA_BUSY) {
        code = die(8, "shown: pixels kept, the pace moved on, a third view BUSY (the third's answer)", third);
        goto out;
    }

    // A new size past the plane's class: stale while placed, ready after a
    // hide, on a new plane.
    os64_media_player_size(pr.r.player, 960, 540);
    f = WAIT_FACT(&pr, 3000, f_.plane_stale);
    if (!f.plane_stale || f.plane_ready) {
        code = die(8, "a new size while placed did not go stale", f.plane_ready);
        goto out;
    }
    uint32_t old_name = f.plane_name;
    if (hide(win, old_name) != 0) {
        code = die(8, "the plane would not be hidden", hide(win, old_name));
        goto out;
    }
    f = WAIT_FACT(&pr, 3000, f_.plane_ready && f_.plane_pic_w == 960);
    if (!f.plane_ready || f.plane_pic_w != 960 || f.plane_recreates != 1 || f.plane_cap_w < 960 ||
        f.plane_name == old_name) {
        code = die(8, "hidden, the new size did not come ready on a new plane (recreates)", (int64_t)f.plane_recreates);
        goto out;
    }
    if (place(win, f.plane_name, (os64_gui_rect_t){0, 0, 960, 540}, (os64_gui_rect_t){0, 0, 400, 300}) != 0) {
        code = die(8, "the new plane would not be placed", 0);
        goto out;
    }
    os64_sleep(300);

    // Closed with the plane placed: finished after the closing bound, the
    // plane taken out by the kernel as the show thread ends.
    int64_t t0 = os64_micros();
    os64_media_player_close(pr.r.player);
    for (int i = 0; i < 1000 && !__atomic_load_n(&s_finished, __ATOMIC_ACQUIRE); i++)
        os64_sleep(10);   // up to ten seconds: past the closing bound
    int64_t took = (os64_micros() - t0) / 1000;
    pr.r.player = NULL;     // closed (and freed, once finished)
    plane_rig_close(&pr);   // it only lets the output go
    int64_t planes_after = gui_planes();
    os64_printf("playertest: closed with the plane placed: finished after %ld ms, planes %ld then %ld\n", took,
                planes_before, planes_after);
    if (!__atomic_load_n(&s_finished, __ATOMIC_ACQUIRE) || took < OS64_MEDIA_SHOW_CLOSE_BOUND_US / 1000 ||
        planes_after != planes_before) {
        code = die(8, "closed placed: no finish after the bound, or the plane left behind (ms)", took);
        goto destroy;
    }

    // Refused: the window's four slots are taken, so the player has none,
    // and `frame` is the caller's again.
    int64_t mine[4];
    for (int i = 0; i < 4; i++)
        mine[i] = os64_gui_plane_create(win, 64, 48);
    if (!plane_rig_open(&pr)) {
        code = die(8, "the second player would not open", 0);
        goto release;
    }
    rc = os64_media_player_plane(pr.r.player, win);
    f = facts(&pr.r);
    os64_media_player_play(pr.r.player);
    bool framed = false;
    for (int i = 0; i < 200 && !framed; i++) {
        framed = os64_media_player_frame(pr.r.player, os64_micros() + 16667, 16667, &fr);
        os64_sleep(10);
    }
    plane_rig_close(&pr);
    if (rc != OS64_MEDIA_NO_PLANE || f.plane_state != OS64_MEDIA_PLANE_REFUSED || f.plane_refusal == 0 || !framed)
        code = die(8, "with no slot: refused, its reason, and frame the caller's again (the call)", rc);
release:
    for (int i = 0; i < 4; i++)
        if (mine[i] > 0)
            os64_gui_plane_destroy(win, (uint32_t)mine[i]);
destroy:
    if (os64_gui_window_destroy(win) != 0 && code == 0)
        code = die(8, "the window would not go after its planes", 0);
    return code;
out:
    plane_rig_close(&pr);
    goto destroy;
}

// An MP3 with no header naming its length, from a source that does not
// know its size: the probe cannot say how long it is, and the end does.
typedef struct {
    size_t at;
} mp3_t;

static int64_t mp3_read(void *ctx, uint8_t *out, size_t cap)
{
    mp3_t *m = ctx;
    size_t left = sizeof(kSineMp3) - m->at, n = cap < left ? cap : left;
    os64_memcpy(out, kSineMp3 + m->at, n);
    m->at += n;
    return (int64_t)n;
}

static int step_length_at_end(void)
{
    mp3_t mp3 = {0};
    rig_t r;
    os64_memset(&r, 0, sizeof(r));
    s_finished = false;
    if (os64_media_output_create(&r.output) != 0)
        return die(6, "the output would not start", 0);
    os64_media_player_config_t cfg = {
        .source = {&mp3, mp3_read, NULL, -1, false, NULL},
        .want = OS64_MEDIA_WANT_SOUND,
        .output = r.output,
        .refused = OS64_MEDIA_VOICE_WAIT,
        .loaded = true,
        .look = counted_look,
        .finished = finished,
        .ctx = &r};
    if (os64_media_player_create(&cfg, &r.player) != 0)
        return die(6, "the player would not start", 0);
    if (!wait_state(&r, OS64_MEDIA_PLAYER_PAUSED, 3000))
        return die(6, "the player did not open the MP3", facts(&r).state);
    int64_t probed = facts(&r).length;
    os64_media_player_play(r.player);
    bool ended = wait_state(&r, OS64_MEDIA_PLAYER_ENDED, 5000);
    os64_media_player_facts_t f = facts(&r);
    rig_close(&r);
    os64_printf("playertest: an MP3 of no stated length: %ld at the probe, %ld at the end\n", probed, f.length);
    if (!ended)
        return die(6, "it did not end", f.state);
    if (probed >= 0)
        return die(6, "the probe knew the length, so this proves nothing (the length)", probed);
    if (f.length < 48000 - 1200 || f.length > 48000 + 1200 || f.position != f.length)
        return die(6, "the end did not say the length (frames)", f.length);
    return 0;
}

static int step_seek_while_opening(void)
{
    rig_t r;
    if (!rig_open_gated(&r, true))
        return die(7, "the output or the player would not start", 0);
    int state = facts(&r).state;
    uint32_t g = os64_media_player_seek(r.player, 12000);
    __atomic_store_n(&r.source.gated, false, __ATOMIC_RELEASE);
    if (state != OS64_MEDIA_PLAYER_OPENING) {
        rig_close(&r);
        return die(7, "the player was not OPENING with its reads held back (the state)", state);
    }
    uint64_t end = (uint64_t)os64_micros() + 3000000;
    os64_media_player_facts_t f = facts(&r);
    while ((uint64_t)os64_micros() < end && (int32_t)(f.seeked - g) < 0) {
        os64_sleep(10);
        f = facts(&r);
    }
    s_heard_n = 0;
    s_dropped = false;
    os64_media_player_play(r.player);
    bool ended = wait_state(&r, OS64_MEDIA_PLAYER_ENDED, 5000);
    listen_for(300);
    rig_close(&r);
    int64_t start = heard_start();
    os64_printf("playertest: a seek while opening: generation %u, landed %u at frame %ld; heard %u frames\n", g,
                f.seeked, f.position, s_heard_n);
    if ((int32_t)(f.seeked - g) < 0 || f.position != 12000)
        return die(7, "a seek asked while opening did not land where it was asked (the frame)", f.position);
    // Frame 12,000 is a crossing (250 whole cycles), so heard_start finds it.
    uint32_t heard = start < 0 ? 0 : s_heard_n - (uint32_t)start;
    if (!ended || s_dropped || heard < FRAMES - 12000)
        return die(7, "it did not play on from the seek to the end (frames heard)", heard);
    for (uint32_t i = 0; i < FRAMES - 12000; i++)
        if (s_heard[(start + i) * 2] != s_samples[(12000 + i) * 2])
            return die(7, "what was heard is not the file from the seek (the frame)", i);
    return 0;
}

int main(void)
{
    char sink[32];
    if (sound_guard_audible(sink, sizeof(sink))) {
        os64_printf("playertest: SKIP: %s can be heard, and this test sets the volume\n", sink);
        return (int)STEP(0xFE);
    }
    int volume = sound_guard_volume_get();
    sound_guard_volume_set(100);
    make_wav();
    s_tap = (int32_t)os64_open("/dev/audiotap", "r");
    int code = s_tap < 0 ? die(1, "the tap would not open", s_tap) : 0;
    if (code == 0)
        code = step_whole();
    if (code == 0)
        code = step_loop();
    if (code == 0)
        code = step_wait();
    if (code == 0)
        code = step_idle();
    if (code == 0)
        code = step_picture_loop("/tests/pages/loop-sound-short.mp4");
    if (code == 0)
        code = step_picture_loop("/tests/pages/loop-picture-short.mp4");
    if (code == 0)
        code = step_picture_loop("/tests/pages/loop-sound-tiny.mp4");
    if (code == 0)
        code = step_picture_loop("/tests/pages/loop-sound-near.mp4");
    if (code == 0)
        code = step_length_at_end();
    if (code == 0)
        code = step_seek_while_opening();
    if (code == 0)
        code = step_plane();
    if (s_tap >= 0)
        os64_close(s_tap);
    if (volume >= 0)
        sound_guard_volume_set(volume);
    if (code != 0)
        return code;
    os64_printf("playertest: all steps passed\n");
    return PLAYERTEST_OK;
}
