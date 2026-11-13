// playtest — /bin/play, end to end, with the tap listening: a WAV written
// here goes in, and what the sink was given is checked against it.
//
//   1. a quarter second of a 1 kHz sine at 44.1 kHz, MONO, played: the tap
//      hears a quarter second at 48 kHz (and the resampler's latency ahead
//      of it), both channels the same, and the same sine: fitted by least
//      squares, under two counts in 12,000 left over (resampletest's bar);
//   2. the same at 48 kHz stereo, which needs no resampler: the tap hears
//      the file's samples exactly;
//   3. a file that is not a WAV, one that is not 16-bit, and one that is
//      not there, are refused with exit 2 and nothing reaches the device.
//
// It takes no arguments. The file it plays is its own, written under a name
// no person would pick (/home/playtest-<task>.tmp.wav) and removed at the
// end; play's own messages go to /dev/null, since three of them are
// refusals this test asks for and would read as failures.
//
// Exit codes: 0x91A10000 success, 0x91A100NN the step that failed (99: it
// was given arguments).

#include "os64/os64.h"
#include "os64/audio.h"
#include "../resampletest/checks.h"

#define PLAYTEST_OK 0x91A10000
#define STEP(n)     (0x91A10000 | (n))

static int die(int step, const char *why, int64_t got)
{
    os64_printf("playtest: FAIL step %d: %s (got %ld)\n", step, why, got);
    return (int)STEP(step);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// A WAV at `path`: `frames` of the tone, with a chunk a player must skip
// (odd-sized, so its pad byte is in play too) between the format and the
// sound.
static bool write_wav(const char *path, uint32_t channels, uint32_t rate, uint32_t bits, uint32_t frames)
{
    static uint8_t file[64 + 2 * 2 * 12000];
    uint32_t data = frames * channels * 2, at = 0;
    os64_memcpy(file, "RIFF", 4); put32(file + 4, 4 + 24 + 14 + 8 + data); os64_memcpy(file + 8, "WAVE", 4);
    at = 12;
    os64_memcpy(file + at, "fmt ", 4); put32(file + at + 4, 16);
    put16(file + at + 8, 1); put16(file + at + 10, channels); put32(file + at + 12, rate);
    put32(file + at + 16, rate * channels * 2); put16(file + at + 20, channels * 2); put16(file + at + 22, bits);
    at += 24;
    os64_memcpy(file + at, "LIST", 4); put32(file + at + 4, 5); os64_memcpy(file + at + 8, "hello\0", 6);
    at += 14;
    os64_memcpy(file + at, "data", 4); put32(file + at + 4, data);
    at += 8;
    for (uint32_t i = 0; i < frames; i++)
    {
        int16_t v = (int16_t)lrint(RC_AMPLITUDE * sin(2.0 * RC_PI * RC_TONE_HZ * (double)i / (double)rate));
        put16(file + at, (uint16_t)v); at += 2;
        if (channels == 2) { put16(file + at, (uint16_t)(int16_t)-v); at += 2; }
    }
    int64_t h = os64_open(path, "w");
    if (h < 0)
        return false;
    bool whole = os64_write((int32_t)h, file, at) == (int64_t)at;
    os64_close((int32_t)h);
    return whole;
}

static int32_t gTap = -1, gPos = -1;
static uint32_t gHeard;            // sound frames in rc_out, stereo
static bool gTapDropped;           // the tap lost records: what was heard is not the whole of it

// Collects the player on its own thread, so the main one can go on reading
// the tap while it plays: an unread tap fills in about a second.
typedef struct { int64_t kid; int32_t code; volatile bool gone; } waiter_t;

static int64_t collect(void *arg)
{
    waiter_t *w = arg;
    w->code = -1;
    os64_wait(w->kid, &w->code);
    __atomic_store_n(&w->gone, true, __ATOMIC_RELEASE);
    return 0;
}

static void listen(uint64_t ms)
{
    static uint8_t buf[8 * OS64_AUDIO_TAP_READ_MIN];
    int64_t n = os64_read_for(gTap, buf, sizeof(buf), ms);
    for (size_t at = 0; n > 0 && at < (size_t)n;)
    {
        os64_audio_tap_t h;
        os64_memcpy(&h, buf + at, sizeof(h));
        at += sizeof(h);
        if (h.dropped != 0)
            gTapDropped = true;
        if (!h.silence && gHeard + h.frames <= RC_MAX)
        {
            os64_memcpy(rc_out + gHeard * 2, buf + at, h.frames * 4);
            gHeard += h.frames;
        }
        at += h.frames * 4;
    }
}

// Run play on `path` and collect what the tap hears, until the player has
// exited AND the sound it left queued has played out. Returns play's exit
// code, or -1 when it could not be run or did not end within a minute.
static int play(const char *path)
{
    gHeard = 0;
    gTapDropped = false;
    waiter_t w = { 0, -1, false };
    int64_t quiet = os64_open("/dev/null", "w");
    if (quiet < 0)
        return -1;
    w.kid = os64_spawn_redirected("/bin/play", (char *[]){ "/bin/play", (char *)path, 0 },
                                  -1, (int32_t)quiet, (int32_t)quiet, 0);
    os64_close((int32_t)quiet);   // the child has its own hold
    if (w.kid < 0)
        return -1;
    int64_t thread = os64_thread(collect, &w);
    if (thread < 0)
        return -1;
    for (int turns = 0; turns < 1200; turns++)
    {
        listen(50);
        if (!__atomic_load_n(&w.gone, __ATOMIC_ACQUIRE))
            continue;
        os64_audio_position_t p;
        os64_memset(&p, 0, sizeof(p));
        (void)os64_read(gPos, &p, sizeof(p));
        if (p.state == OS64_AUDIO_IDLE)
        {
            listen(0);   // what the last pull left at the tap
            os64_thread_join((int32_t)thread, NULL);
            os64_close((int32_t)thread);
            return w.code;
        }
    }
    return -1;   // the thread is left waiting; the test is failing anyway
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        os64_printf("playtest: takes no arguments; it writes, plays and removes a file of its own.\n"
                    "          To play a file of yours: play -v file.wav\n");
        return (int)STEP(99);
    }
    char wav[64], missing[64];
    os64_snprintf(wav, sizeof(wav), "/home/playtest-%lu.tmp.wav", (unsigned long)os64_taskid());
    os64_snprintf(missing, sizeof(missing), "/home/playtest-%lu.none.wav", (unsigned long)os64_taskid());

    gPos = (int32_t)os64_open("/dev/audiopos", "r");
    gTap = (int32_t)os64_open("/dev/audiotap", "r");
    if (gPos < 0 || gTap < 0)
        return die(1, "the position or the tap would not open", gPos < 0 ? gPos : gTap);

    // Whatever an earlier program left: gone, and a known volume. The tap
    // hears the sound after the gain.
    int64_t ctl = os64_open("/dev/audioctl", "w");
    if (ctl >= 0) { os64_write((int32_t)ctl, "flush\n", 6); os64_close((int32_t)ctl); }
    ctl = os64_open("/dev/volume", "w");
    if (ctl < 0 || os64_write((int32_t)ctl, "100\n", 4) != 4)
        return die(1, "could not set the volume to 100", ctl);
    os64_close((int32_t)ctl);

    // 1. Mono at 44.1 kHz, through the resampler.
    if (!write_wav(wav, 1, 44100, 16, 11025))
        return die(1, "could not write the test file", 0);
    int code = play(wav);
    if (code != 0)
        return die(1, "play did not exit 0", code);
    if (gTapDropped)
        return die(1, "the tap dropped records: this test was held off for too long to judge", 0);
    // 11,025 frames and the 40 the resampler holds, at 48,000 for 44,100.
    if (gHeard < 12043 - 4 || gHeard > 12043 + 4)
        return die(1, "the tap did not hear a quarter second and the latency", gHeard);
    for (uint32_t i = 0; i < gHeard; i++)
        if (rc_out[i * 2] != rc_out[i * 2 + 1])
            return die(1, "a mono file's two channels differ", i);
    double amplitude, residual;
    rc_fit(2, 0, 48000, 2000, gHeard - 2000, &amplitude, &residual);
    os64_printf("playtest: fitted amplitude %ld, residual %ld hundredths of a count\n",
                lrint(amplitude), lrint(residual * 100.0));
    if (fabs(amplitude - RC_AMPLITUDE) > RC_AMPLITUDE / 1000.0 || residual > 2.0)
        return die(1, "what the tap heard is not the file's sine", lrint(residual * 100.0));

    // 2. Stereo at the device's rate: sample for sample.
    if (!write_wav(wav, 2, 48000, 16, 12000))
        return die(2, "could not write the test file", 0);
    code = play(wav);
    if (gTapDropped)
        return die(2, "the tap dropped records: this test was held off for too long to judge", 0);
    if (code != 0 || gHeard != 12000)
        return die(2, "play did not play the 48 kHz file whole", code != 0 ? code : (int)gHeard);
    for (uint32_t i = 0; i < 12000; i++)
    {
        int16_t v = (int16_t)lrint(RC_AMPLITUDE * sin(2.0 * RC_PI * RC_TONE_HZ * (double)i / 48000.0));
        if (rc_out[i * 2] != v || rc_out[i * 2 + 1] != (int16_t)-v)
            return die(2, "a 48 kHz file did not arrive sample for sample", i);
    }

    // 3. Refusals.
    int64_t h = os64_open(wav, "w");
    os64_write((int32_t)h, "this is not a wave file at all", 30);
    os64_close((int32_t)h);
    code = play(wav);
    if (code != 2 || gHeard != 0)
        return die(3, "a file that is not a WAV was not refused with exit 2", code);
    if (!write_wav(wav, 2, 48000, 8, 100))
        return die(3, "could not write the test file", 0);
    code = play(wav);
    if (code != 2 || gHeard != 0)
        return die(3, "an 8-bit file was not refused with exit 2", code);
    code = play(missing);
    if (code != 2)
        return die(3, "a missing file was not refused with exit 2", code);

    os64_unlink(wav);
    os64_close(gTap);
    os64_close(gPos);
    os64_printf("playtest: all steps passed\n");
    return (int)PLAYTEST_OK;
}
