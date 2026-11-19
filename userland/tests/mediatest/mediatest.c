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
//      frames left after it, and what it returns is what the unbroken
//      decode gave there (the decoder and the resampler both run in)
//  11  a seek in a 44.1 kHz WAV, which goes through the resampler, returns
//      what an unbroken decode gives at that frame, not a ramp up from
//      silence
//   7  a six-channel WAV, which names no layout, is read as 5.1 and folds
//      to stereo by BS.775's weights, normalised
//   8  a source that cannot seek but knows its length plays whole, and a
//      seek is refused by name
//  12  a remote source's seek in a stream longer than libmedia's reading
//      buffer asks for the byte its time is estimated at, and lands within
//      two packets of the frame asked for
//   9  bytes that are no media are refused by name
//  13  the types it answers for (canPlayType's words): AAC and MP4 maybe,
//      MP4 naming AAC probably, MP4 naming H.264 (avc1, avc3, alone or with
//      their six hex digits) probably, H.264 strings that do not parse and
//      a video codec not built no, and a type it has no reader for no; and
//      the sound types' list names audio/mp4 and no video type
//  14  an MP4 with a picture first and AAC after (avtest's sine_m4a.h)
//      opens as MP4 ("mov", the reader's first name) and plays its sound:
//      about one second of the sine at 48 kHz stereo, holding the energy
//      the host's FFmpeg decodes the bytes to
//
// The picture side, on /tests/pages/bbb-480p-3s-av.mp4 (92 pictures of
// 854x480 at 30 a second, the pixel aspect 1280:1281, and AAC sound, both
// from the file's start):
//  15  it opens for both: sound and picture playable, h264, 854x480 coded
//      and 853 wide as shown, 30 a second, no rotation; os64_media_decode
//      is refused on it, as pulling sound alone would drop the pictures
//  16  os64_media_next gives 92 pictures, picture i at 1,600 i (a 30th of
//      a second) lasting 1,600, and the sound beside them, contiguous from
//      frame 0 and as long as the file
//  17  the first 30 pictures, converted bit-exact to 640x360 by a scaler,
//      give avbench's CRC for the same pictures (a434a9b9): libmedia
//      converts as the libav fixture does, on any CPU's routines
//  18  a seek to 72,800 gives first the picture showing there (at 72,000)
//      and then the 46 after it, and the sound from 72,800
//  19  after the end, a seek to 0 makes it decode again from picture 0
//  20  a sound-only open of the same file reads no picture packet, says
//      the picture is absent, and decodes as many frames of sound as the
//      open for both (the host harness compares the samples themselves)
//  21  a second thread converts and releases every picture while the
//      first decodes; once the decoder has run again, none is held
//  22  the picture-only clip (bbb-480p-3s.mp4): open for both or for the
//      picture, it opens with the sound absent; open for the sound, it is
//      refused, NO_SOUND
//  23  a FAST scaler enlarging /tests/pages/video-tiny.mp4's 16-wide
//      pictures to 64x64 and 128x96: the case where FFmpeg's fast bilinear
//      would run code it writes into the heap, which os64 ends with a
//      no-execute fault (media_scale.c, FAST IS FAST ONLY WHEN SHRINKING).
//      Passing is surviving, with every conversion answered 0
//
// Exit codes: 0x3ED10000 success, 0x3ED100NN the step that failed.

#include "os64/os64.h"
#include "os64/media.h"
#include "os64/thread.h"

#include "../avtest/sine_m4a.h"
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
    size_t seek_max;    // the furthest offset a seek asked for
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
    if (m->at > m->seek_max) m->seek_max = m->at;
    return (int64_t)offset;
}

static os64_media_source_t source_of(Memory *m, const uint8_t *data, size_t size, bool seekable)
{
    *m = (Memory){ data, size, 0, 0 };
    // The size is given either way, as a download that cannot seek still
    // knows its length.
    os64_media_source_t s = { m, memory_read, seekable ? memory_seek : NULL, (int64_t)size, false };
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
    // The whole decode, kept, for step 6 to compare a seek against.
    static int16_t whole[48000 * 2 + 64];
    int64_t got = 0, n;
    while ((n = os64_media_decode(m, whole + 2 * got, (uint32_t)(48000 + 32 - got))) > 0) got += n;
    Tally t;
    os64_memset(&t, 0, sizeof(t));
    t.result = (int)n;
    for (int64_t i = 0; i < got; i++) {
        int16_t l = whole[2 * i], rr = whole[2 * i + 1];
        if (l != rr) t.mismatched_sides++;
        if (t.frames > 0 && (t.last < 0) != (l < 0)) t.crossings++;
        t.last = l;
        t.sum_squares += (double)l * l;
        t.frames++;
    }
    int failed = check_mp3_sound(&t, 2, 3);
    if (failed) { os64_media_close(m); return failed; }

    // 6a. A seek into the middle of a packet whose frame draws on the bit
    // reservoir (5,200 output frames: Codex's case on #595) gives back the
    // unbroken decode's samples.
    int16_t after[256 * 2];
    r = os64_media_seek(m, 5200);
    n = r == 0 ? os64_media_decode(m, after, 256) : r;
    if (n != 256) { say("MP3 seek (result, frames)", r, (long)n); os64_media_close(m); return FAIL(6); }
    for (int i = 0; i < 256; i++) {
        int d = after[2 * i] - whole[2 * (5200 + i)];
        if (d < -2 || d > 2) {
            say("MP3 seek differs from the unbroken decode (frame, by)", 5200 + i, d);
            os64_media_close(m);
            return FAIL(6);
        }
    }

    // 6b. Seek to half a second: what is left is the other half.
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

// A remote source's seek goes to the byte its time is estimated at instead
// of reading its way there. The sine's audio frames (its ID3 tag and its
// Info frame left out, so the length is the bit rate's) are laid end to
// end 32 times, about 270 KiB: far past libmedia's 32 KiB reading buffer
// and the 32 KiB past it that FFmpeg reads its way across rather than
// seek, either of which would never ask the source. A seek two seconds from
// the end asks the source for a byte past half the stream, and what is
// left after it is those two seconds, within two packets (a packet is
// 1,152 media frames, 1,254 of the output's).
#define LONG_COPIES 32
static int step_remote_seek(void)
{
    static uint8_t stream[sizeof(kSineMp3) * LONG_COPIES];
    size_t tag = 10 + ((size_t)(kSineMp3[6] & 0x7F) << 21 | (size_t)(kSineMp3[7] & 0x7F) << 14 |
                       (size_t)(kSineMp3[8] & 0x7F) << 7 | (size_t)(kSineMp3[9] & 0x7F));
    // MPEG-1 Layer III: 144 * bit rate / rate bytes, and one for padding.
    static const uint32_t kKbps[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    const uint8_t *h = kSineMp3 + tag;
    size_t info = 144 * kKbps[h[2] >> 4] * 1000 / 44100 + ((h[2] >> 1) & 1);
    size_t frames = sizeof(kSineMp3) - tag - info, size = 0;
    for (int i = 0; i < LONG_COPIES; i++, size += frames)
        os64_memcpy(stream + size, kSineMp3 + tag + info, frames);

    Memory mem;
    os64_media_source_t src = source_of(&mem, stream, size, true);
    os64_media_t *m;
    int r = os64_media_open(&src, &m, NULL);
    if (r != 0) { say("remote open, whole (result)", r, 0); return FAIL(12); }
    Tally whole;
    tally_all(m, &whole);
    os64_media_close(m);
    src = source_of(&mem, stream, size, true);
    src.remote = true;
    r = os64_media_open(&src, &m, NULL);
    if (r != 0 || whole.result != 0 || whole.frames < 24 * 48000) {
        say("remote open (result, whole frames)", r, (long)whole.frames);
        if (r == 0) os64_media_close(m);
        return FAIL(12);
    }
    mem.seek_max = 0;
    r = os64_media_seek(m, whole.frames - 2 * 48000);
    Tally t;
    tally_all(m, &t);
    os64_media_close(m);
    if (r != 0 || t.result != 0 || mem.seek_max < size / 2 || t.frames < 2 * 48000 - 2600 ||
        t.frames > 2 * 48000 + 2600) {
        say("remote seek (furthest byte asked, frames after)", (long)mem.seek_max, (long)t.frames);
        return FAIL(12);
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

static int step_types(void)
{
    static const struct { const char *type; int answer; } kAsk[] = {
        {"audio/mp4", OS64_MEDIA_TYPE_MAYBE}, {"audio/x-m4a", OS64_MEDIA_TYPE_MAYBE}, {"audio/aac", OS64_MEDIA_TYPE_MAYBE},
        {"video/mp4", OS64_MEDIA_TYPE_MAYBE}, {"audio/mp4; codecs=\"mp4a.40.2\"", OS64_MEDIA_TYPE_PROBABLY},
        {"video/mp4; codecs=\"avc1.4d400d,mp4a.40.2\"", OS64_MEDIA_TYPE_PROBABLY},
        {"video/mp4; codecs=\"avc3.64001F\"", OS64_MEDIA_TYPE_PROBABLY}, {"video/mp4; codecs=avc1", OS64_MEDIA_TYPE_PROBABLY},
        {"video/mp4; codecs=\"avc1.4d40\"", OS64_MEDIA_TYPE_NO}, {"video/mp4; codecs=\"avc1.4d400g\"", OS64_MEDIA_TYPE_NO},
        {"video/mp4; codecs=\"hvc1.1.6.L93.B0\"", OS64_MEDIA_TYPE_NO},
        {"audio/mpeg", OS64_MEDIA_TYPE_MAYBE}, {"video/webm", OS64_MEDIA_TYPE_NO}, {"audio/ogg", OS64_MEDIA_TYPE_NO},
    };
    for (size_t i = 0; i < sizeof(kAsk) / sizeof(kAsk[0]); i++) {
        int got = os64_media_type_supported(kAsk[i].type);
        if (got != kAsk[i].answer) {
            os64_printf("mediatest: type '%s' answered %d, not %d\n", kAsk[i].type, got, kAsk[i].answer);
            return FAIL(13);
        }
    }
    // The sound types' list names AAC's and leaves the video type out,
    // which plays only its sound and so is no sound a page is made for.
    static char list[512];
    (void)os64_media_type_list(list, sizeof(list));
    bool aac = false, video = false;
    for (const char *p = list; *p != '\0';) {
        const char *end = p;
        while (*end != '\0' && *end != ',') end++;
        size_t n = (size_t)(end - p);
        aac |= n == 9 && os64_memcmp(p, "audio/mp4", 9) == 0;
        video |= n >= 6 && os64_memcmp(p, "video/", 6) == 0;
        p = *end == ',' ? end + 2 : end;
    }
    if (!aac || video) {
        os64_printf("mediatest: the sound types' list: %s\n", list);
        return FAIL(13);
    }
    return 0;
}

static int step_m4a(void)
{
    Memory mem;
    os64_media_source_t src = source_of(&mem, kSineM4a, sizeof(kSineM4a), true);
    os64_media_t *m;
    os64_media_info_t info;
    int r = os64_media_open(&src, &m, &info);
    if (r != 0 || !os64_streq(info.format, "mov") || info.rate != 44100 || info.channels != 1) {
        say("MP4 open (result, rate)", r, r == 0 ? (long)info.rate : 0);
        if (r == 0) os64_media_close(m);
        return FAIL(14);
    }
    Tally t;
    tally_all(m, &t);
    os64_media_close(m);
    // avtest's bounds on the samples (one AAC frame of priming either way
    // between FFmpeg releases), at 48 kHz.
    int64_t most = ((int64_t)SINE_M4A_SAMPLES + 1024) * 48000 / 44100 + 64;
    double hz = (double)t.crossings / 2.0 * 48000.0 / (double)(t.frames > 0 ? t.frames : 1);
    // Its energy, which resampling to 48 kHz scales by 48000/44100 (the
    // same power in more samples) and the priming trimmed does not move.
    double energy = t.sum_squares / (32768.0 * 32768.0) * 1e6 * 44100.0 / 48000.0;
    if (t.result != 0 || t.frames < 48000 || t.frames > most || t.mismatched_sides != 0 || hz < 435 || hz > 445
        || energy < SINE_M4A_ENERGY_PPM * 0.98 || energy > SINE_M4A_ENERGY_PPM * 1.02) {
        say("MP4 sound (frames, energy per million)", (long)t.frames, (long)energy);
        return FAIL(14);
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

// ------------------------------------------------------------ the picture side

#define CLIP_AV "/tests/pages/bbb-480p-3s-av.mp4"
#define CLIP_PICTURE "/tests/pages/bbb-480p-3s.mp4"
#define PICTURES 92
#define PERIOD 1600                   // output frames in a 30th of a second
#define CLIP_SOUND 147168             // the AAC track's 3.066 s, at 48 kHz
#define EXACT_CRC 0xa434a9b9u         // avbench -n 30 -x 640x360 -e on the same pictures

static uint32_t s_crc_table[256];

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (s_crc_table[1] == 0)
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            s_crc_table[i] = c;
        }
    while (n--) crc = s_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

typedef struct {
    int32_t handle;
    os64_media_t *m;
    os64_media_info_t info;
    int result;
} Clip;

static void clip_open(Clip *c, const char *path, uint32_t want)
{
    os64_memset(c, 0, sizeof(*c));
    c->handle = (int32_t)os64_open(path, "r");
    if (c->handle < 0) { c->result = OS64_MEDIA_IO; return; }
    os64_media_source_t src;
    os64_media_file_source(c->handle, &src);
    c->result = os64_media_open_for(&src, want, &c->m, &c->info);
}

static void clip_close(Clip *c)
{
    if (c->m) os64_media_close(c->m);
    if (c->handle >= 0) os64_close(c->handle);
    c->m = NULL;
    c->handle = -1;
}

typedef struct {
    long pictures, sound_frames, first_picture_at, picture_gaps, sound_gaps;
    int64_t sound_next;               // where the next sound should start
    uint32_t exact_crc;
    int result;
} Pull;

// Everything os64_media_next gives until the end. With `exact`, the first
// 30 pictures are converted by it and folded into exact_crc.
static void pull_all(os64_media_t *m, os64_media_scaler_t *exact, uint32_t *pixels, int64_t sound_from, Pull *p)
{
    static int16_t pcm[4096 * 2];
    os64_memset(p, 0, sizeof(*p));
    p->first_picture_at = -1;
    p->sound_next = sound_from;
    p->exact_crc = 0xFFFFFFFFu;
    int64_t expect_at = -1;
    for (;;) {
        os64_media_got_t got;
        int r = os64_media_next(m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got);
        if (r != 0) { p->result = r; return; }
        if (got.kind == OS64_MEDIA_GOT_END) return;
        if (got.kind == OS64_MEDIA_GOT_SOUND) {
            if (got.at != p->sound_next) p->sound_gaps++;
            p->sound_next = got.at + got.frames;
            p->sound_frames += got.frames;
            continue;
        }
        os64_media_picture_t *pic = got.picture;
        if (p->first_picture_at < 0) {
            p->first_picture_at = pic->at;
            expect_at = pic->at;
        }
        if (pic->at != expect_at || pic->duration != PERIOD) p->picture_gaps++;
        expect_at = pic->at + PERIOD;
        if (exact != NULL && p->pictures < 30) {
            if (os64_media_scaler_convert(exact, pic, pixels, 640 * 4) != 0) p->result = -100;
            p->exact_crc = crc_update(p->exact_crc, (const uint8_t *)pixels, 640 * 360 * 4);
        }
        p->pictures++;
        os64_media_picture_release(m, pic);
    }
}

static int steps_pictures(void)
{
    Clip c;
    clip_open(&c, CLIP_AV, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    const os64_media_info_t *i = &c.info;
    int16_t one[2];
    if (c.result != 0 || i->sound != OS64_MEDIA_PLAYABLE || i->picture != OS64_MEDIA_PLAYABLE ||
        !os64_streq(i->picture_codec, "h264") || i->coded_width != 854 || i->coded_height != 480 ||
        i->width != 853 || i->height != 480 || i->rate_num != 30 || i->rate_den != 1 || i->rotation != 0 ||
        os64_media_decode(c.m, one, 1) != OS64_MEDIA_BAD_ARGS) {
        say("both open (result, shown width)", c.result, (long)i->width);
        clip_close(&c);
        return FAIL(15);
    }

    os64_media_scaler_t *exact;
    static uint32_t pixels[640 * 360];
    if (os64_media_scaler_create(640, 360, OS64_MEDIA_SCALE_EXACT, &exact) != 0) {
        clip_close(&c);
        return FAIL(17);
    }
    Pull p;
    pull_all(c.m, exact, pixels, 0, &p);
    os64_media_scaler_destroy(exact);
    if (p.result != 0 || p.pictures != PICTURES || p.first_picture_at != 0 || p.picture_gaps != 0 ||
        p.sound_gaps != 0 || p.sound_frames < CLIP_SOUND - 1024 || p.sound_frames > CLIP_SOUND + 1024) {
        say("whole pull (pictures, sound frames)", p.pictures, p.sound_frames);
        clip_close(&c);
        return FAIL(16);
    }
    if ((p.exact_crc ^ 0xFFFFFFFFu) != EXACT_CRC) {
        say("exact conversion's crc (got, wanted)", (long)(p.exact_crc ^ 0xFFFFFFFFu), (long)EXACT_CRC);
        clip_close(&c);
        return FAIL(17);
    }
    long whole_sound = p.sound_frames;

    // 18: 72,800 is half a picture past picture 45.
    int r = os64_media_seek(c.m, 72800);
    pull_all(c.m, NULL, NULL, 72800, &p);
    if (r != 0 || p.result != 0 || p.first_picture_at != 72000 || p.pictures != PICTURES - 45 ||
        p.picture_gaps != 0 || p.sound_gaps != 0) {
        say("seek (first picture at, pictures after)", p.first_picture_at, p.pictures);
        clip_close(&c);
        return FAIL(18);
    }
    // 19: the end is behind it; a seek starts it again.
    r = os64_media_seek(c.m, 0);
    pull_all(c.m, NULL, NULL, 0, &p);
    if (r != 0 || p.result != 0 || p.first_picture_at != 0 || p.pictures != PICTURES) {
        say("replay (first picture at, pictures)", p.first_picture_at, p.pictures);
        clip_close(&c);
        return FAIL(19);
    }
    clip_close(&c);

    // 20: the same file for its sound alone.
    clip_open(&c, CLIP_AV, OS64_MEDIA_WANT_SOUND);
    Tally t;
    if (c.result == 0) tally_all(c.m, &t);
    os64_media_counts_t counts;
    os64_media_counts(c.m, &counts);
    bool sound_only = c.result == 0 && c.info.picture == OS64_MEDIA_ABSENT && c.info.sound == OS64_MEDIA_PLAYABLE &&
                      t.result == 0 && t.frames == whole_sound && counts.picture_packets == 0 &&
                      counts.sound_packets > 0;
    clip_close(&c);
    if (!sound_only) {
        say("sound only (picture packets, frames)", (long)counts.picture_packets, c.result == 0 ? (long)t.frames : c.result);
        return FAIL(20);
    }
    return 0;
}

// 21: pictures handed to a second thread, which converts and releases them.
#define HANDOFF 8
static struct {
    os64_lock_t lock;
    os64_media_picture_t *ring[HANDOFF];
    uint32_t head, count;
    bool ended;
    bool gone;                         // the converter could not start: nothing will empty the ring
    os64_media_t *m;
    long converted, failed;
} s_handoff;

static int64_t convert_thread(void *arg)
{
    (void)arg;
    os64_media_scaler_t *s;
    static uint32_t pixels[320 * 180];
    if (os64_media_scaler_create(320, 180, OS64_MEDIA_SCALE_FAST, &s) != 0) {
        os64_lock_acquire(&s_handoff.lock);
        s_handoff.gone = true;
        os64_lock_release(&s_handoff.lock);
        return -1;
    }
    for (;;) {
        os64_lock_acquire(&s_handoff.lock);
        os64_media_picture_t *p = NULL;
        bool ended = s_handoff.ended;
        if (s_handoff.count > 0) {
            p = s_handoff.ring[s_handoff.head];
            s_handoff.head = (s_handoff.head + 1) % HANDOFF;
            s_handoff.count--;
        }
        os64_lock_release(&s_handoff.lock);
        if (p == NULL) {
            if (ended) break;
            os64_yield();
            continue;
        }
        if (os64_media_scaler_convert(s, p, pixels, 320 * 4) == 0) s_handoff.converted++;
        else s_handoff.failed++;
        os64_media_picture_release(s_handoff.m, p);
    }
    os64_media_scaler_destroy(s);
    return 0;
}

static int step_handoff(void)
{
    Clip c;
    clip_open(&c, CLIP_AV, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    if (c.result != 0) { say("handoff open (result)", c.result, 0); return FAIL(21); }
    os64_memset(&s_handoff, 0, sizeof(s_handoff));
    s_handoff.m = c.m;
    int64_t tid = os64_thread(convert_thread, NULL);
    if (tid < 0) {
        clip_close(&c);
        say("handoff: no second thread (result)", (long)tid, 0);
        return FAIL(21);
    }
    static int16_t pcm[4096 * 2];
    long given = 0;
    int r = 0;
    bool gone = false;
    while (!gone) {
        os64_media_got_t got;
        if ((r = os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got)) != 0 || got.kind == OS64_MEDIA_GOT_END) break;
        if (got.kind != OS64_MEDIA_GOT_PICTURE) continue;
        for (;;) {
            os64_lock_acquire(&s_handoff.lock);
            bool room = s_handoff.count < HANDOFF;
            gone = s_handoff.gone;
            if (room && !gone) {
                s_handoff.ring[(s_handoff.head + s_handoff.count) % HANDOFF] = got.picture;
                s_handoff.count++;
            }
            os64_lock_release(&s_handoff.lock);
            if (gone) os64_media_picture_release(c.m, got.picture);
            if (room || gone) break;
            os64_yield();
        }
        given += !gone;
    }
    os64_lock_acquire(&s_handoff.lock);
    s_handoff.ended = true;
    os64_lock_release(&s_handoff.lock);
    int64_t joined = -99;
    if (tid >= 0) os64_thread_join((int32_t)tid, &joined);
    // The released pictures go back to the decoder at its next call.
    os64_media_got_t got;
    int again = os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, pcm, 4096, &got);
    os64_media_counts_t counts;
    os64_media_counts(c.m, &counts);
    clip_close(&c);
    if (r != 0 || joined != 0 || given != PICTURES || s_handoff.converted != PICTURES || s_handoff.failed != 0 ||
        again != 0 || got.kind != OS64_MEDIA_GOT_END || counts.pictures_held != 0 || counts.pictures_out != PICTURES) {
        say("handoff (converted, held)", s_handoff.converted, (long)counts.pictures_held);
        return FAIL(21);
    }
    return 0;
}

static int step_fast_enlarge(void)
{
    static const uint32_t kSizes[2][2] = { { 64, 64 }, { 128, 96 } };
    static uint32_t pixels[128 * 96];
    for (int z = 0; z < 2; z++) {
        Clip c;
        clip_open(&c, "/tests/pages/video-tiny.mp4", OS64_MEDIA_WANT_PICTURE);
        os64_media_scaler_t *s = NULL;
        if (c.result != 0 || os64_media_scaler_create(kSizes[z][0], kSizes[z][1], OS64_MEDIA_SCALE_FAST, &s) != 0) {
            say("fast enlarge: open (result)", c.result, 0);
            clip_close(&c);
            return FAIL(23);
        }
        long converted = 0, refused = 0;
        for (;;) {
            os64_media_got_t got;
            if (os64_media_next(c.m, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, NULL, 0, &got) != 0 || got.kind == OS64_MEDIA_GOT_END) break;
            if (os64_media_scaler_convert(s, got.picture, pixels, kSizes[z][0] * 4) == 0) converted++;
            else refused++;
            os64_media_picture_release(c.m, got.picture);
        }
        os64_media_scaler_destroy(s);
        clip_close(&c);
        if (converted == 0 || refused != 0) {
            say("fast enlarge (converted, refused)", converted, refused);
            return FAIL(23);
        }
    }
    return 0;
}

static int step_picture_only(void)
{
    Clip c;
    static const uint32_t kWants[2] = { OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, OS64_MEDIA_WANT_PICTURE };
    for (int w = 0; w < 2; w++) {
        clip_open(&c, CLIP_PICTURE, kWants[w]);
        bool ok = c.result == 0 && c.info.sound == OS64_MEDIA_ABSENT && c.info.picture == OS64_MEDIA_PLAYABLE;
        Pull p;
        if (ok) {
            pull_all(c.m, NULL, NULL, 0, &p);
            ok = p.result == 0 && p.pictures == PICTURES && p.sound_frames == 0;
        }
        clip_close(&c);
        if (!ok) { say("picture only (want, result)", (long)kWants[w], c.result); return FAIL(22); }
    }
    clip_open(&c, CLIP_PICTURE, OS64_MEDIA_WANT_SOUND);
    int refused = c.result;
    clip_close(&c);
    if (refused != OS64_MEDIA_NO_SOUND) { say("picture only, for sound (result)", refused, 0); return FAIL(22); }
    return 0;
}

// 24: `accept`. The sound taken alone runs to its end, every picture packet
// read on the way kept undecoded (the clip is far under the bound); then the
// pictures taken alone are every one, in order, and then the end.
static int step_accept(void)
{
    static int16_t pcm[4096 * 2];
    Clip c;
    clip_open(&c, CLIP_AV, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE);
    long sound = 0, pictures = 0, gaps = 0;
    int64_t sound_next = 0, expect_at = 0;
    int r = c.result, sound_end = -1, picture_end = -1;
    os64_media_got_t got;
    while (r == 0) {
        r = os64_media_next(c.m, OS64_MEDIA_WANT_SOUND, pcm, 4096, &got);
        if (r != 0 || got.kind != OS64_MEDIA_GOT_SOUND) { sound_end = got.kind; break; }
        if (got.at != sound_next) gaps++;
        sound_next = got.at + got.frames;
        sound += got.frames;
    }
    os64_media_counts_t counts;
    os64_media_counts(c.m, &counts);
    uint32_t kept = counts.picture_listed;
    while (r == 0) {
        r = os64_media_next(c.m, OS64_MEDIA_WANT_PICTURE, NULL, 0, &got);
        if (r != 0 || got.kind != OS64_MEDIA_GOT_PICTURE) { picture_end = got.kind; break; }
        if (got.picture->at != expect_at) gaps++;
        expect_at = got.picture->at + PERIOD;
        pictures++;
        os64_media_picture_release(c.m, got.picture);
    }
    clip_close(&c);
    if (r != 0 || sound_end != OS64_MEDIA_GOT_END || picture_end != OS64_MEDIA_GOT_END || gaps != 0 ||
        kept != (uint32_t)PICTURES || pictures != PICTURES || sound < CLIP_SOUND - 1024 || sound > CLIP_SOUND + 1024) {
        say("accept (pictures kept, pictures)", (long)kept, pictures);
        return FAIL(24);
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
    if (!failed) failed = step_remote_seek();
    if (!failed) failed = step_noise();
    if (!failed) failed = step_types();
    if (!failed) failed = step_m4a();
    if (!failed) failed = steps_pictures();
    if (!failed) failed = step_handoff();
    if (!failed) failed = step_picture_only();
    if (!failed) failed = step_fast_enlarge();
    if (!failed) failed = step_accept();
    if (failed) {
        os64_printf("mediatest: FAIL step %d\n", failed & 0xFF);
        return failed;
    }
    os64_printf("mediatest: MP3, WAV and AAC in MP4 to 48 kHz stereo, seeks, a 5.1 fold, two threads; "
                "H.264 pictures beside their sound, converted, seeked, handed to another thread, one kind taken at a time; "
                "all steps passed\n");
    return PASS;
}
