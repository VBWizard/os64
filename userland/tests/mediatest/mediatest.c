// mediatest — libmedia.so (os64/media.h) in ring 3, over libav.so and
// libsound.so: what comes out is the stream's format and the media's
// timeline. Each step names itself in the exit code.
//
//  10  FIRST, before anything else has opened a decoder: two threads,
//      released together, open and decode the MP3 at once, and both get it
//      whole (the first opens are the ones that build FFmpeg's shared
//      tables, which libmedia's lock must cover)
//   1  an MP3 (44.1 kHz mono) opens and says what it is
//   2  it decodes to one second of 48 kHz stereo, exactly 48,000 frames
//      (the resampler's delay counted off and its tail drawn out), both
//      sides the same
//   3  the sound is the sine: 440 Hz by its crossings, at the level the
//      host's FFmpeg decodes the bytes to (avtest's sine_mp3.h), within 1%
//   4  a 48 kHz stereo WAV decodes sample for sample
//   5  a seek in it returns the target frame first, sample for sample
//   6  a seek in the MP3 lands on the frame asked for, by the count of
//      frames left after it
//  11  a seek in a 44.1 kHz WAV, which goes through the resampler, returns
//      what an unbroken decode gives at that frame, not a ramp up from
//      silence
//   7  a six-channel WAV, which names no layout, is read as 5.1 and folds
//      to stereo by BS.775's weights, normalised
//   8  a source that cannot seek but knows its length plays whole, and a
//      seek is refused by name
//   9  bytes that are no media are refused by name
//
// Exit codes: 0x3ED10000 success, 0x3ED100NN the step that failed.

#include "os64/os64.h"
#include "os64/media.h"
#include "os64/thread.h"

#include "../avtest/sine_mp3.h"

#define PASS 0x3ED10000
#define FAIL(step) ((int)(PASS | (step)))

static void say(const char *what, long a, long b)
{
    os64_printf("mediatest: %s: %ld, %ld\n", what, a, b);
}

// ------------------------------------------------------------ memory source

typedef struct {
    const uint8_t *data;
    size_t size, at;
} Memory;

static int64_t memory_read(void *ctx, uint8_t *out, size_t cap)
{
    Memory *m = ctx;
    size_t n = m->size - m->at < cap ? m->size - m->at : cap;
    os64_memcpy(out, m->data + m->at, n);
    m->at += n;
    return (int64_t)n;
}

static int64_t memory_seek(void *ctx, uint64_t offset)
{
    Memory *m = ctx;
    if (offset > m->size) return -1;
    m->at = (size_t)offset;
    return (int64_t)offset;
}

static os64_media_source_t source_of(Memory *m, const uint8_t *data, size_t size, bool seekable)
{
    *m = (Memory){ data, size, 0 };
    // The size is given either way, as a download that cannot seek still
    // knows its length.
    os64_media_source_t s = { m, memory_read, seekable ? memory_seek : NULL, (int64_t)size };
    return s;
}

// ------------------------------------------------------------ a WAV

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// A 16-bit PCM WAV of `frames` frames of `channels`; sample(c, i) gives each.
static size_t make_wav(uint8_t *out, uint32_t channels, uint32_t rate, uint32_t frames,
                       int16_t (*sample)(uint32_t c, uint32_t i))
{
    uint32_t data = frames * channels * 2;
    os64_memcpy(out, "RIFF", 4); put32(out + 4, 36 + data); os64_memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16); put16(out + 20, 1); put16(out + 22, channels); put32(out + 24, rate);
    put32(out + 28, rate * channels * 2); put16(out + 32, channels * 2); put16(out + 34, 16);
    os64_memcpy(out + 36, "data", 4); put32(out + 40, data);
    for (uint32_t i = 0; i < frames; i++)
        for (uint32_t c = 0; c < channels; c++)
            put16(out + 44 + (i * channels + c) * 2, (uint16_t)sample(c, i));
    return 44 + data;
}

static int16_t ramp(uint32_t c, uint32_t i) { int16_t v = (int16_t)(i * 3); return c ? (int16_t)~v : v; }
// A slow wave well inside the resampler's band, so what comes out of the
// resampler is close to what went in and a ramp-up would show.
static int16_t wave(uint32_t c, uint32_t i) { (void)c; int32_t p = (int32_t)(i % 400); return (int16_t)((p < 200 ? p : 400 - p) * 100 - 10000); }
static const int16_t kSurround[6] = { 8000, -6000, 4000, 30000, 2000, -1000 };   // FL FR FC LFE BL BR
static int16_t surround(uint32_t c, uint32_t i) { (void)i; return kSurround[c]; }

// ------------------------------------------------------------ decoding everything

typedef struct {
    int64_t frames, crossings, mismatched_sides;
    double sum_squares;
    int16_t last;
    int result;
} Tally;

static void tally_all(os64_media_t *m, Tally *t)
{
    static int16_t pcm[2048 * 2];
    os64_memset(t, 0, sizeof(*t));
    for (;;) {
        int64_t n = os64_media_decode(m, pcm, 2048);
        if (n <= 0) { t->result = (int)n; return; }
        for (int64_t i = 0; i < n; i++) {
            int16_t l = pcm[2 * i], r = pcm[2 * i + 1];
            if (l != r) t->mismatched_sides++;
            if (t->frames > 0 && (t->last < 0) != (l < 0)) t->crossings++;
            t->last = l;
            t->sum_squares += (double)l * l;
            t->frames++;
        }
    }
}

static int check_mp3_sound(const Tally *t, int step_length, int step_sound)
{
    if (t->result != 0 || t->frames != 48000 || t->mismatched_sides != 0) {
        say("MP3 decode (result, frames)", t->result, (long)t->frames);
        return FAIL(step_length);
    }
    double hz = (double)t->crossings / 2.0;
    double ppm = __builtin_sqrt(t->sum_squares / (double)t->frames) / 32768.0 * 1e6;
    if (hz < 438 || hz > 442 || ppm < SINE_MP3_RMS_PPM * 0.99 || ppm > SINE_MP3_RMS_PPM * 1.01) {
        say("MP3 sound (crossings, level per million)", (long)t->crossings, (long)ppm);
        return FAIL(step_sound);
    }
    return 0;
}

// ------------------------------------------------------------ the steps

static int steps_mp3(void)
{
    Memory mem;
    os64_media_source_t src = source_of(&mem, kSineMp3, sizeof(kSineMp3), true);
    os64_media_t *m;
    os64_media_info_t info;
    int r = os64_media_open(&src, &m, &info);
    if (r != 0 || !os64_streq(info.format, "mp3") || info.rate != 44100 || info.channels != 1 || !info.seekable) {
        say("MP3 open (result, rate)", r, r == 0 ? (long)info.rate : 0);
        if (r == 0) os64_media_close(m);
        return FAIL(1);
    }
    Tally t;
    tally_all(m, &t);
    int failed = check_mp3_sound(&t, 2, 3);
    if (failed) { os64_media_close(m); return failed; }

    // 6. Seek to half a second: what is left is the other half.
    r = os64_media_seek(m, 24000);
    tally_all(m, &t);
    os64_media_close(m);
    if (r != 0 || t.result != 0 || t.frames < 24000 - 2 || t.frames > 24000 + 2) {
        say("MP3 seek (result, frames after)", r, (long)t.frames);
        return FAIL(6);
    }
    return 0;
}

static int steps_wav(void)
{
    static uint8_t wav[44 + 9600 * 4];
    size_t size = make_wav(wav, 2, 48000, 9600, ramp);
    Memory mem;
    os64_media_source_t src = source_of(&mem, wav, size, true);
    os64_media_t *m;
    os64_media_info_t info;
    int r = os64_media_open(&src, &m, &info);
    if (r != 0) { say("WAV open (result)", r, 0); return FAIL(4); }
    static int16_t pcm[9600 * 2 + 64];
    int64_t got = 0, n;
    while ((n = os64_media_decode(m, pcm + 2 * got, (uint32_t)(9600 + 32 - got))) > 0) got += n;
    for (int64_t i = 0; i < got; i++)
        if (pcm[2 * i] != ramp(0, (uint32_t)i) || pcm[2 * i + 1] != ramp(1, (uint32_t)i)) {
            say("WAV sample differs (frame, value)", (long)i, pcm[2 * i]);
            os64_media_close(m);
            return FAIL(4);
        }
    if (n < 0 || got != 9600) { say("WAV decode (result, frames)", (long)n, (long)got); os64_media_close(m); return FAIL(4); }

    r = os64_media_seek(m, 1000);
    n = os64_media_decode(m, pcm, 16);
    bool ok = r == 0 && n == 16;
    for (int i = 0; ok && i < 16; i++) ok = pcm[2 * i] == ramp(0, (uint32_t)(1000 + i));
    os64_media_close(m);
    if (!ok) { say("WAV seek (result, first frame's sample)", r, n > 0 ? pcm[0] : -1); return FAIL(5); }
    return 0;
}

static int step_resampled_seek(void)
{
    static uint8_t wav[44 + 8820 * 2];
    size_t size = make_wav(wav, 1, 44100, 8820, wave);
    Memory mem;
    os64_media_source_t src = source_of(&mem, wav, size, true);
    os64_media_t *m;
    int r = os64_media_open(&src, &m, NULL);
    if (r != 0) { say("44.1 kHz WAV open (result)", r, 0); return FAIL(11); }
    static int16_t whole[9600 * 2 + 64];
    int64_t got = 0, n;
    while ((n = os64_media_decode(m, whole + 2 * got, (uint32_t)(9600 + 32 - got))) > 0) got += n;
    // 4,960 output frames is 4,557 media frames, exactly 31 of the 44.1 to
    // 48 kHz ratio's cycles of 147: a seek that started its resampler right
    // there, with no run-in, would have nothing before the target to work
    // from, and its first frames would ramp up from silence.
    enum { AT = 4960 };
    int16_t after[256 * 2];
    r = os64_media_seek(m, AT);
    n = r == 0 ? os64_media_decode(m, after, 256) : r;
    os64_media_close(m);
    if (got < AT + 256 || n != 256) { say("resampled seek (frames before, after)", (long)got, (long)n); return FAIL(11); }
    for (int i = 0; i < 256; i++) {
        int d = after[2 * i] - whole[2 * (AT + i)];
        if (d < -2 || d > 2) {
            say("resampled seek differs from the unbroken decode (frame, by)", AT + i, d);
            return FAIL(11);
        }
    }
    return 0;
}

static int step_surround(void)
{
    static uint8_t wav[44 + 480 * 12];
    size_t size = make_wav(wav, 6, 48000, 480, surround);
    Memory mem;
    os64_media_source_t src = source_of(&mem, wav, size, true);
    os64_media_t *m;
    os64_media_info_t info;
    int r = os64_media_open(&src, &m, &info);
    int16_t pcm[64 * 2];
    int64_t n = r == 0 ? os64_media_decode(m, pcm, 64) : r;
    if (r == 0) os64_media_close(m);
    // L = (FL + h*FC + h*BL) / (1 + 2h), R likewise; LFE left out.
    double h = 0.70710678118654752440;
    double l = (kSurround[0] + h * kSurround[2] + h * kSurround[4]) / (1 + 2 * h);
    double rr = (kSurround[1] + h * kSurround[2] + h * kSurround[5]) / (1 + 2 * h);
    if (n != 64 || info.channels != 6 || __builtin_fabs(pcm[0] - l) > 1 || __builtin_fabs(pcm[1] - rr) > 1) {
        say("5.1 fold (left, right)", n > 0 ? pcm[0] : n, n > 0 ? pcm[1] : 0);
        return FAIL(7);
    }
    return 0;
}

static int step_unseekable(void)
{
    Memory mem;
    os64_media_source_t src = source_of(&mem, kSineMp3, sizeof(kSineMp3), false);
    os64_media_t *m;
    os64_media_info_t info;
    int r = os64_media_open(&src, &m, &info);
    if (r != 0) { say("unseekable open (result)", r, 0); return FAIL(8); }
    int refused = os64_media_seek(m, 100);
    Tally t;
    tally_all(m, &t);
    os64_media_close(m);
    if (info.seekable || refused != OS64_MEDIA_CANNOT_SEEK || t.result != 0 || t.frames != 48000) {
        say("unseekable (seek result, frames)", refused, (long)t.frames);
        return FAIL(8);
    }
    return 0;
}

static int step_noise(void)
{
    static uint8_t noise[8192];
    uint32_t x = 0x9E3779B9;
    for (size_t i = 0; i < sizeof(noise); i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; noise[i] = (uint8_t)x; }
    Memory mem;
    os64_media_source_t src = source_of(&mem, noise, sizeof(noise), true);
    os64_media_t *m = (os64_media_t *)1;
    int r = os64_media_open(&src, &m, NULL);
    if (r != OS64_MEDIA_NOT_MEDIA || m != NULL) {
        say("noise (result)", r, 0);
        if (r == 0) os64_media_close(m);
        return FAIL(9);
    }
    return 0;
}

// Both threads wait here until the main thread lets them go together, so
// their opens overlap as nearly as two threads can be made to.
static volatile int gGo;

static int64_t decode_thread(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&gGo, __ATOMIC_ACQUIRE)) os64_yield();
    Memory mem;
    os64_media_source_t src = source_of(&mem, kSineMp3, sizeof(kSineMp3), true);
    os64_media_t *m;
    if (os64_media_open(&src, &m, NULL) != 0) return -1;
    int16_t pcm[1024 * 2];
    int64_t frames = 0, n;
    while ((n = os64_media_decode(m, pcm, 1024)) > 0) frames += n;
    os64_media_close(m);
    return n < 0 ? n : frames;
}

static int step_threads(void)
{
    int64_t a = os64_thread(decode_thread, NULL), b = os64_thread(decode_thread, NULL);
    __atomic_store_n(&gGo, 1, __ATOMIC_RELEASE);
    int64_t ra = -99, rb = -99;
    if (a >= 0) os64_thread_join((int32_t)a, &ra);
    if (b >= 0) os64_thread_join((int32_t)b, &rb);
    if (ra != 48000 || rb != 48000) {
        say("two threads (frames each)", (long)ra, (long)rb);
        return FAIL(10);
    }
    return 0;
}

int main(void)
{
    int failed = step_threads();      // first: see step 10 above
    if (!failed) failed = steps_mp3();
    if (!failed) failed = steps_wav();
    if (!failed) failed = step_resampled_seek();
    if (!failed) failed = step_surround();
    if (!failed) failed = step_unseekable();
    if (!failed) failed = step_noise();
    if (failed) {
        os64_printf("mediatest: FAIL step %d\n", failed & 0xFF);
        return failed;
    }
    os64_printf("mediatest: MP3 and WAV to 48 kHz stereo, seeks, a 5.1 fold, two threads; all steps passed\n");
    return PASS;
}
