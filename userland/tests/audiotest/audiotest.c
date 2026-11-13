// audiotest — sound as a file (os64/audio.h), with nothing listening but the
// tap. Each rule of the seam is one step here, through the real syscalls,
// and each step has its own exit code:
//
//    1. one open of /dev/audio and of /dev/audiotap at a time, each refused
//       with OS64_AUDIO_BUSY; the position is a record, served short if
//       asked short; the control file refuses what it does not know;
//    2. half a second of a ramp, written with the ordinary blocking write,
//       arrives at the tap whole, in order, at 48,000 frames a second; the
//       write waited for the ring; the position counted it;
//    3. the ring running dry is one underrun, shown as silence at the tap,
//       and the ramp picks up unbroken at the next write;
//    4. paused, nothing is taken; a write with no patience answers
//       OS64_ERR_TIMEOUT against a full ring, and one with some waits it
//       out first;
//    5. a second thread is refused while one is parked in a write, and a
//       flush wakes the parked one to OS64_AUDIO_FLUSHED; what was queued
//       never reaches the tap, and the generation moves;
//    6. a signal the program handles ends a parked write with
//       OS64_INTERRUPTED;
//    7. a close under a parked write wakes it; closing a paused stream
//       throws its sound away;
//    8. /dev/volume at 50 is a quarter of every sample, exactly;
//    9. closing with sound queued lets it play, and an open made at once
//       follows it: two generations at the tap, in order, and the second
//       owner's position counts only its own;
//   10. a child born with the stream as its output plays through it, and
//       the handle is listed as `audio`;
//   11. what a read of the position costs, against micros().
//
// The tap is read through every wait the test makes, because an open stream
// with nothing in it is still given silence a hundred times a second, and a
// tap nobody reads fills in about a second.
//
// `audiotest` runs it; `audiotest feed` and `audiotest poke <task>` are the
// helpers it spawns.
//
// Exit codes: 0xA0D10000 success, 0xA0D100NN the step that failed.

#include "os64/os64.h"
#include "os64/audio.h"
#include "os64/signal.h"

#define AUDIOTEST_OK 0xA0D10000
#define STEP(n)      (0xA0D10000 | (n))

#define SETTLE_MS  200       // enough for a thread to be parked before the main thread acts
#define PROMPT_US  400000    // a wake that took longer waited for the kernel's backstop
#define RING       9600u     // frames in the stream's ring

static int die(int step, const char *why, int64_t got)
{
    os64_printf("audiotest: FAIL step %d: %s (got %ld)\n", step, why, got);
    return (int)STEP(step);
}

static int32_t gAudio = -1, gTap = -1, gPos = -1;

// ── The ramp ────────────────────────────────────────────────────────────

static int16_t ramp_left(uint32_t i)  { return (int16_t)(i % 30000u); }
static int16_t ramp_right(uint32_t i) { return (int16_t)~ramp_left(i); }

static int16_t gPcm[2 * 24000];

static void fill_ramp(uint32_t from, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        gPcm[i * 2] = ramp_left(from + i);
        gPcm[i * 2 + 1] = ramp_right(from + i);
    }
}

// ── The tap's log ───────────────────────────────────────────────────────
// Every record read since the last tap_forget(): its sound appended to
// gHeard, and what the headers said checked as they arrive.

#define HEARD_MAX 60000u
static int16_t  gHeard[2 * HEARD_MAX];
static uint32_t gHeardGen[HEARD_MAX];
static uint32_t gHeardFrames;
static uint64_t gSilenceFrames;
static uint64_t gNextFirst;
static bool     gHaveFirst;
static const char *gTapFault;
static int64_t  gFirstSoundUs, gLastSoundUs;
static uint32_t gLastSoundFrames;

static void tap_forget(void)
{
    gHeardFrames = 0;
    gSilenceFrames = 0;
    gFirstSoundUs = gLastSoundUs = 0;
}

static uint8_t gTapBuf[8 * OS64_AUDIO_TAP_READ_MIN];

// Read the tap for up to `ms`, taking in whatever it has.
static void tap_listen(uint64_t ms)
{
    int64_t until = os64_micros() + (int64_t)ms * 1000;
    for (;;) {
        int64_t left = until - os64_micros();
        int64_t n = os64_read_for(gTap, gTapBuf, sizeof(gTapBuf), left > 0 ? (uint64_t)(left / 1000) : 0);
        if (n == OS64_ERR_TIMEOUT) {
            if (left <= 0)
                return;
            continue;
        }
        if (n <= 0) {
            gTapFault = "a tap read failed";
            return;
        }
        for (size_t at = 0; at < (size_t)n;) {
            os64_audio_tap_t h;
            os64_memcpy(&h, gTapBuf + at, sizeof(h));
            at += sizeof(h);
            if (h.frames == 0 || h.frames > OS64_AUDIO_TAP_RECORD_FRAMES || at + h.frames * 4 > (size_t)n) {
                gTapFault = "a tap record was not whole";
                return;
            }
            if (h.dropped != 0)
                gTapFault = "the tap dropped records";
            if (gHaveFirst && h.first != gNextFirst)
                gTapFault = "the tap's frame indices have a gap";
            gHaveFirst = true;
            gNextFirst = h.first + h.frames;
            if (h.silence) {
                gSilenceFrames += h.frames;
            } else if (gHeardFrames + h.frames <= HEARD_MAX) {
                os64_memcpy(gHeard + gHeardFrames * 2, gTapBuf + at, h.frames * 4);
                for (uint32_t i = 0; i < h.frames; i++)
                    gHeardGen[gHeardFrames + i] = h.generation;
                if (gFirstSoundUs == 0)
                    gFirstSoundUs = (int64_t)h.micros;
                gLastSoundUs = (int64_t)h.micros;
                gLastSoundFrames = h.frames;
                gHeardFrames += h.frames;
            } else {
                gTapFault = "more sound than the test has room to hold";
            }
            at += h.frames * 4;
        }
    }
}

static os64_audio_position_t position(void)
{
    os64_audio_position_t p;
    os64_memset(&p, 0, sizeof(p));
    (void)os64_read(gPos, &p, sizeof(p));
    return p;
}

// Listen until the sink has taken `frames` of this generation, or 3 s.
static bool until_taken(uint64_t frames)
{
    for (int i = 0; i < 300; i++) {
        if (position().taken >= frames)
            return true;
        tap_listen(10);
    }
    return false;
}

static bool until_state(uint32_t state)
{
    for (int i = 0; i < 300; i++) {
        if (position().state == state)
            return true;
        tap_listen(10);
    }
    return false;
}

// The sound heard since tap_forget() is frames [from, from+frames) of the
// ramp, scaled by `quarter` (volume 50) or not, under generation `gen`,
// starting at heard index `at`.
static bool heard_ramp(uint32_t at, uint32_t from, uint32_t frames, uint32_t gen, bool quarter)
{
    if (at + frames > gHeardFrames)
        return false;
    for (uint32_t i = 0; i < frames; i++) {
        int l = ramp_left(from + i), r = ramp_right(from + i);
        if (quarter) { l /= 4; r /= 4; }   // C's division: toward zero, as the gain is
        if (gHeard[(at + i) * 2] != l || gHeard[(at + i) * 2 + 1] != r || gHeardGen[at + i] != gen)
            return false;
    }
    return true;
}

static int64_t control(const char *path, const char *text)
{
    int64_t h = os64_open(path, "w");
    if (h < 0)
        return h;
    int64_t n = os64_write((int32_t)h, text, os64_strlen(text));
    os64_close((int32_t)h);
    return n;
}

// Pause the stream and fill its ring to the brim.
static bool pause_and_fill(void)
{
    if (control("/dev/audioctl", "pause\n") < 0)
        return false;
    fill_ramp(0, RING);
    uint32_t queued = position().queued;
    return os64_write_for(gAudio, gPcm, (RING - queued) * 4, 0) == (int64_t)((RING - queued) * 4);
}

// ── Threads and helpers the steps start ─────────────────────────────────

static int64_t write_one_frame_forever(void *arg)
{
    static const int16_t frame[2] = { 77, -78 };
    return os64_write(*(int32_t *)arg, frame, sizeof(frame));
}

static volatile int gHandled;
static void on_interrupt(int signo) { gHandled = signo; }

static uint64_t parse_number(const char *s)
{
    uint64_t n = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        n = n * 10 + (uint64_t)(*s - '0');
    return n;
}

static int poke_main(const char *task_text)
{
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", (unsigned long)parse_number(task_text));
    os64_sleep(SETTLE_MS);              // let the parent park first
    int64_t ctl = os64_open(path, "w");
    if (ctl < 0)
        return 1;
    int64_t wrote = os64_write((int32_t)ctl, "interrupt", 9);
    os64_close((int32_t)ctl);
    return wrote == 9 ? 0 : 2;
}

// The child of step 10: its standard output is the stream.
static int feed_main(void)
{
    fill_ramp(7000, 480);
    return os64_write(1, gPcm, 480 * 4) == 480 * 4 ? 0 : 1;
}

static bool handles_list_audio(void)
{
    static char text[8192];
    int64_t h = os64_open("/proc/self/handles", "r");
    if (h < 0)
        return false;
    size_t have = 0;
    for (;;) {
        int64_t n = os64_read((int32_t)h, text + have, sizeof(text) - 1 - have);
        if (n <= 0)
            break;
        have += (size_t)n;
    }
    os64_close((int32_t)h);
    for (size_t i = 0; i + 6 <= have; i++)
        if (text[i] == '\t' && text[i + 1] == 'a' && text[i + 2] == 'u' && text[i + 3] == 'd' &&
            text[i + 4] == 'i' && text[i + 5] == 'o')
            return true;
    return false;
}

int main(int argc, char **argv)
{
    if (argc == 2 && os64_streq(argv[1], "feed"))
        return feed_main();
    if (argc == 3 && os64_streq(argv[1], "poke"))
        return poke_main(argv[2]);

    int64_t r;
    os64_audio_position_t p;

    // 1. The names.
    gPos = (int32_t)os64_open("/dev/audiopos", "r");
    gTap = (int32_t)os64_open("/dev/audiotap", "r");
    if (gPos < 0 || gTap < 0)
        return die(1, "the position or the tap would not open", gPos < 0 ? gPos : gTap);
    // Whatever an earlier program left: gone, and a known volume.
    control("/dev/audioctl", "flush\n");
    control("/dev/volume", "100\n");
    gAudio = (int32_t)os64_open("/dev/audio", "w");
    if (gAudio < 0)
        return die(1, "/dev/audio would not open", gAudio);
    if ((r = os64_open("/dev/audio", "w")) != OS64_AUDIO_BUSY)
        return die(1, "a second open of /dev/audio was not refused BUSY", r);
    if ((r = os64_open("/dev/audiotap", "r")) != OS64_AUDIO_BUSY)
        return die(1, "a second open of /dev/audiotap was not refused BUSY", r);
    if ((r = os64_open("/dev/audio", "r")) >= 0)
        return die(1, "/dev/audio opened for reading", r);
    p = position();
    if (p.size != sizeof(p) || p.state != OS64_AUDIO_WAITING || p.rate != OS64_AUDIO_RATE ||
        p.accepted != 0 || p.taken != 0 || p.queued != 0)
        return die(1, "a fresh stream's position is not a fresh stream's", p.state);
    uint32_t gen = p.generation;
    os64_audio_position_t shortp;
    os64_memset(&shortp, 0xFF, sizeof(shortp));
    if ((r = os64_read(gPos, &shortp, 8)) != 8 || shortp.size != sizeof(p) || shortp.generation != gen ||
        shortp.state != 0xFFFFFFFFu)
        return die(1, "a short read of the position was not served short", r);
    if (control("/dev/audioctl", "format s16le 2 48000\n") < 0)
        return die(1, "the one format was refused", 0);
    if (control("/dev/audioctl", "format s16le 2 44100\n") >= 0 || control("/dev/audioctl", "louder\n") >= 0 ||
        control("/dev/audioctl", "resume\n") >= 0 || control("/dev/audioctl", "pause now\n") >= 0)
        return die(1, "the control file took something it should refuse", 0);
    uint8_t small[64];
    if ((r = os64_read_for(gTap, small, sizeof(small), 0)) >= 0 || r == OS64_ERR_TIMEOUT)
        return die(1, "a tap read too small for a record was not refused", r);

    // 2. Half a second of the ramp.
    tap_listen(20);
    tap_forget();
    fill_ramp(0, 24000);
    int64_t before = os64_micros();
    r = os64_write(gAudio, gPcm, 24000 * 4);
    int64_t wrote_us = os64_micros() - before;
    if (r != 24000 * 4)
        return die(2, "the blocking write did not take everything", r);
    // 24,000 frames into a ring of 9,600: the last of them could not go in
    // until 14,400 had been taken, which is 0.3 s of playing.
    if (wrote_us < 250000 || wrote_us > 900000)
        return die(2, "the blocking write did not wait for the ring", wrote_us);
    if (!until_taken(24000))
        return die(2, "the sink never took the half second", (int64_t)position().taken);
    tap_listen(30);
    if (gTapFault)
        return die(2, gTapFault, 0);
    if (gHeardFrames != 24000 || !heard_ramp(0, 0, 24000, gen, false))
        return die(2, "the tap did not hear the ramp whole and in order", gHeardFrames);
    // From the first record's pull to the last's, all but the last record's
    // frames were played: that span is the rate.
    int64_t span = gLastSoundUs - gFirstSoundUs;
    int64_t want = (int64_t)(24000 - gLastSoundFrames) * 1000000 / 48000;
    if (span < want - 40000 || span > want + 40000)
        return die(2, "the ramp was not taken at 48,000 frames a second", span);
    p = position();
    if (p.accepted != 24000 || p.taken != 24000 || p.heard != 24000 || p.generation != gen)
        return die(2, "the position did not count the ramp", (int64_t)p.taken);

    // 3. Dry: one underrun, silence at the tap, and the ramp goes on.
    if (!until_state(OS64_AUDIO_STARVED))
        return die(3, "an emptied stream is not starved", position().state);
    tap_listen(60);
    p = position();
    if (p.underruns != 1 || gSilenceFrames == 0)
        return die(3, "running dry was not one underrun with silence at the tap", (int64_t)p.underruns);
    tap_forget();
    fill_ramp(24000, 4800);
    if ((r = os64_write(gAudio, gPcm, 4800 * 4)) != 4800 * 4)
        return die(3, "the write after the dry spell failed", r);
    if (!until_taken(28800))
        return die(3, "the sink never took the second piece", (int64_t)position().taken);
    tap_listen(60);
    if (gTapFault)
        return die(3, gTapFault, 0);
    if (gHeardFrames != 4800 || !heard_ramp(0, 24000, 4800, gen, false))
        return die(3, "the ramp did not pick up unbroken after the dry spell", gHeardFrames);
    if (position().underruns != 2)
        return die(3, "the second dry spell was not the second underrun", (int64_t)position().underruns);

    // 4. Paused.
    if (!pause_and_fill())
        return die(4, "a paused stream would not fill", position().queued);
    p = position();
    if (p.state != OS64_AUDIO_PAUSED || p.queued != RING)
        return die(4, "a paused, filled stream does not say so", p.state);
    if (control("/dev/audioctl", "pause\n") >= 0)
        return die(4, "a second pause was not refused", 0);
    tap_listen(20);
    tap_forget();
    uint64_t silence_before = gSilenceFrames;
    tap_listen(150);
    os64_audio_position_t later = position();
    if (later.taken != p.taken || later.micros != p.micros || gHeardFrames != 0 || gSilenceFrames != silence_before)
        return die(4, "a paused stream was still being taken", (int64_t)later.taken);
    if ((r = os64_write_for(gAudio, gPcm, 4, 0)) != OS64_ERR_TIMEOUT)
        return die(4, "a write with no patience did not answer TIMEOUT against a full ring", r);
    before = os64_micros();
    r = os64_write_for(gAudio, gPcm, 4, 100);
    if (r != OS64_ERR_TIMEOUT || os64_micros() - before < 80000)
        return die(4, "a write with 100 ms of patience did not wait it out", r);

    // 5. Parked, refused, flushed.
    int32_t held = gAudio;
    int64_t writer = os64_thread(write_one_frame_forever, &held);
    if (writer < 0)
        return die(5, "no writer thread", writer);
    os64_sleep(SETTLE_MS);
    if ((r = os64_write_for(gAudio, gPcm, 4, 0)) != OS64_AUDIO_BUSY)
        return die(5, "a second writer was not refused BUSY while one is parked", r);
    before = os64_micros();
    if (control("/dev/audioctl", "flush\n") < 0)
        return die(5, "flush was refused", 0);
    int64_t answer = -99;
    os64_thread_join((int32_t)writer, &answer);
    int64_t woke_us = os64_micros() - before;
    os64_close((int32_t)writer);
    if (answer != OS64_AUDIO_FLUSHED || woke_us > PROMPT_US)
        return die(5, "a flush did not wake the parked writer to FLUSHED, promptly", answer);
    p = position();
    if (p.generation != gen + 1 || p.queued != 0 || p.accepted != 0 || p.state != OS64_AUDIO_PAUSED)
        return die(5, "a flush did not empty the stream under a new generation", p.generation);
    gen = p.generation;
    if (control("/dev/audioctl", "resume\n") < 0)
        return die(5, "resume was refused", 0);
    fill_ramp(50000, 480);
    if ((r = os64_write(gAudio, gPcm, 480 * 4)) != 480 * 4 || !until_taken(480))
        return die(5, "the stream did not play after the flush", r);
    tap_listen(60);
    if (gTapFault)
        return die(5, gTapFault, 0);
    if (gHeardFrames != 480 || !heard_ramp(0, 50000, 480, gen, false))
        return die(5, "flushed sound reached the tap, or the new sound did not", gHeardFrames);

    // 6. A handled signal ends a parked write.
    if (!pause_and_fill())
        return die(6, "a paused stream would not fill", position().queued);
    if (os64_signal_set_handler(OS64_SIGINT, on_interrupt) < 0)
        return die(6, "the SIGINT handler was refused", 0);
    char me[24];
    os64_snprintf(me, sizeof(me), "%lu", (unsigned long)os64_taskid());
    int64_t kid = os64_spawn("/tests/audiotest", (char *[]){ "/tests/audiotest", "poke", me, 0 });
    if (kid < 0)
        return die(6, "could not spawn the sender", kid);
    r = os64_write_for(gAudio, gPcm, 4, 10000);
    if (r != OS64_INTERRUPTED || gHandled != OS64_SIGINT)
        return die(6, "a handled signal did not end the parked write with INTERRUPTED", r);
    int32_t code = -1;
    while (os64_wait(kid, &code) == OS64_INTERRUPTED)
        ;
    os64_signal_set_handler(OS64_SIGINT, OS64_SIG_DEFAULT);
    if (code != 0)
        return die(6, "the sender could not write our ctl", code);

    // 7. Closed under a parked write; and a paused stream's close.
    held = gAudio;
    writer = os64_thread(write_one_frame_forever, &held);
    if (writer < 0)
        return die(7, "no writer thread", writer);
    os64_sleep(SETTLE_MS);
    before = os64_micros();
    os64_close(gAudio);
    answer = -99;
    os64_thread_join((int32_t)writer, &answer);
    woke_us = os64_micros() - before;
    os64_close((int32_t)writer);
    if (answer >= 0 || woke_us > PROMPT_US)
        return die(7, "a close under a parked writer did not end its write, promptly", answer);
    p = position();
    if (p.state != OS64_AUDIO_IDLE || p.queued != 0 || p.generation != gen + 1)
        return die(7, "closing a paused stream did not throw its sound away", p.state);
    gAudio = (int32_t)os64_open("/dev/audio", "w");
    if (gAudio < 0)
        return die(7, "the stream would not open again", gAudio);
    gen = position().generation;

    // 8. Volume.
    char volume[16];
    int64_t vh = os64_open("/dev/volume", "r");
    r = vh < 0 ? vh : os64_read((int32_t)vh, volume, sizeof(volume));
    if (r != 4 || volume[0] != '1' || volume[1] != '0' || volume[2] != '0' || volume[3] != '\n' ||
        os64_read((int32_t)vh, volume, sizeof(volume)) != 0)
        return die(8, "/dev/volume did not read as a line and then the end", r);
    os64_close((int32_t)vh);
    if (control("/dev/volume", "101\n") >= 0 || control("/dev/volume", "loud\n") >= 0 ||
        control("/dev/volume", "5 0\n") >= 0)
        return die(8, "/dev/volume took something it should refuse", 0);
    if (control("/dev/volume", "50\n") < 0)
        return die(8, "/dev/volume refused 50", 0);
    tap_listen(20);
    tap_forget();
    fill_ramp(29000, 2000);   // across the ramp's wrap, so both signs and both ends
    if ((r = os64_write(gAudio, gPcm, 2000 * 4)) != 2000 * 4 || !until_taken(2000))
        return die(8, "the stream did not play at volume 50", r);
    tap_listen(60);
    control("/dev/volume", "100\n");
    if (gTapFault)
        return die(8, gTapFault, 0);
    if (gHeardFrames != 2000 || !heard_ramp(0, 29000, 2000, gen, true))
        return die(8, "volume 50 was not a quarter of every sample", gHeardFrames);

    // 9. Closed with sound queued, and opened at once.
    tap_listen(20);
    tap_forget();
    fill_ramp(100, 4800);
    if ((r = os64_write(gAudio, gPcm, 4800 * 4)) != 4800 * 4)
        return die(9, "the first owner's write failed", r);
    os64_close(gAudio);
    gAudio = (int32_t)os64_open("/dev/audio", "w");
    if (gAudio < 0)
        return die(9, "an open while the last owner's sound drains was refused", gAudio);
    p = position();
    if (p.generation != gen + 1 || p.accepted != 0 || p.taken != 0 || p.queued == 0 ||
        p.state != OS64_AUDIO_PLAYING)
        return die(9, "the second owner's position does not show the first's sound ahead of it", p.queued);
    fill_ramp(9000, 480);
    if ((r = os64_write(gAudio, gPcm, 480 * 4)) != 480 * 4 || !until_taken(480))
        return die(9, "the second owner's sound was never taken", r);
    tap_listen(60);
    if (gTapFault)
        return die(9, gTapFault, 0);
    if (gHeardFrames != 5280 || !heard_ramp(0, 100, 4800, gen, false) ||
        !heard_ramp(4800, 9000, 480, gen + 1, false))
        return die(9, "the tap did not hear both owners, in order, each under its generation", gHeardFrames);
    p = position();
    if (p.taken != 480 || p.accepted != 480)
        return die(9, "the second owner's position counted the first owner's frames", (int64_t)p.taken);
    gen = p.generation;

    // 10. A child whose output is the stream.
    if (!handles_list_audio())
        return die(10, "/proc/self/handles does not list an audio handle", 0);
    tap_listen(20);
    tap_forget();
    kid = os64_spawn_redirected("/tests/audiotest", (char *[]){ "/tests/audiotest", "feed", 0 },
                                -1, gAudio, -1, 0);
    if (kid < 0)
        return die(10, "a spawn with the stream as the child's output was refused", kid);
    // Listening while the child loads and plays, and only then collecting
    // it: a wait that blocked here would leave the tap unread.
    bool played = until_taken(960);
    code = -1;
    os64_wait(kid, &code);
    if (code != 0 || !played)
        return die(10, "the child could not write the stream it was born with", code);
    tap_listen(60);
    if (gTapFault)
        return die(10, gTapFault, 0);
    if (gHeardFrames != 480 || !heard_ramp(0, 7000, 480, gen, false))
        return die(10, "the child's sound did not reach the tap", gHeardFrames);
    // Still ours after the child let go of its copy: one open, two handles.
    if ((r = os64_open("/dev/audio", "w")) != OS64_AUDIO_BUSY)
        return die(10, "the child's exit closed the stream under its parent", r);

    // 11. What the position costs. Paused, so that nothing is given to the
    // sink, and so to the tap, while nobody reads it.
    if (control("/dev/audioctl", "pause\n") < 0)
        return die(11, "pause was refused", 0);
    enum { ASKS = 100000 };
    before = os64_micros();
    for (int i = 0; i < ASKS; i++)
        (void)os64_micros();
    int64_t micros_us = os64_micros() - before;
    before = os64_micros();
    for (int i = 0; i < ASKS; i++)
        (void)os64_read(gPos, &p, sizeof(p));
    int64_t position_us = os64_micros() - before;
    os64_printf("audiotest: a position read costs %ld ns, a micros() %ld ns\n",
                position_us * 1000 / ASKS, micros_us * 1000 / ASKS);
    tap_listen(20);
    if (gTapFault)
        return die(11, gTapFault, 0);

    os64_close(gAudio);
    os64_close(gTap);
    os64_close(gPos);
    os64_printf("audiotest: all steps passed\n");
    return (int)AUDIOTEST_OK;
}
