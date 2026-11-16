// play — play sound files through /dev/audio (os64/audio.h).
//
//     play song.mp3            play it; Ctrl+C stops it at once
//     play a.mp3 b.mp3 c.wav   one after another
//     play -v song.wav         and say what each file is
//
// The files: anything libmedia reads (os64/media.h): MP3, AAC, and WAV of
// 8, 16, 24 or 32 bits or 32-bit floating point, a-law or mu-law, at any
// rate and channel count. libmedia hands back the device's own format
// (16-bit stereo at 48 kHz), so this program only moves sound. The ordinary
// blocking write is the clock: it returns when the device has taken the
// sound, so this program runs for the length of the songs with no timing of
// its own.
//
// The media keys (os64/mediakeys.h, MEDIA_KEYS.md): play holds a session,
// says `playing` and `paused`, and between two pieces of sound looks for a
// press without waiting. Play and Pause pause and resume it; Next goes to
// the next file (after the last, play ends); Previous goes back a file
// within the first three seconds of one, and to the start of this one
// later; Stop ends it. Without a session (the machine's are all taken) it
// plays on, deaf to the keys.
//
// It does not wait for the last of the sound before exiting. A close leaves
// what is queued to play out (a fifth of a second at most), and a `play`
// started within that follows it without a cut.
//
// Exit codes: 0 played; 1 usage; 2 a file would not open, is not sound this
// plays, or is damaged past playing (the others still play); 3 the device
// refused or failed; 130 stopped by Ctrl+C.

#include "os64/os64.h"
#include "os64/audio.h"
#include "os64/media.h"
#include "os64/mediakeys.h"
#include "os64/signal.h"

#define CHUNK_FRAMES 4096u
// Previous within this much of a file's start goes back a file.
#define PREVIOUS_GOES_BACK_FRAMES (3u * OS64_AUDIO_RATE)

static volatile int gStop;
static void on_interrupt(int signo) { (void)signo; gStop = 1; }

// What the keys asked of the file playing.
// GO_SKIP: the file would not play; go on the way we were going.
typedef enum { GO_ON, GO_NEXT, GO_PREVIOUS, GO_RESTART, GO_STOP, GO_SKIP } go_t;

typedef struct
{
    int32_t audio;
    int32_t ctl;      // /dev/audioctl, or -1
    int32_t keys;     // the session, or -1
    int exit_code;
} player_t;

static void control(player_t *p, const char *verb)
{
    if (p->ctl >= 0)
        os64_write(p->ctl, verb, os64_strlen(verb));
}

static void say(player_t *p, const char *state)
{
    if (p->keys >= 0)
        os64_write(p->keys, state, os64_strlen(state));
}

// One press, waiting for it as long as `wait_ms` (0: only look). False when
// there was none (or the wait ended without one).
static bool next_press(player_t *p, uint64_t wait_ms, uint32_t *key)
{
    if (p->keys < 0)
        return false;
    os64_media_key_t k;
    if (os64_read_for(p->keys, &k, sizeof(k), wait_ms) != (int64_t)sizeof(k))
        return false;
    *key = k.key;
    return true;
}

// What a press asks, `played` frames into the file. PLAY while playing is
// nothing: the keys may give a press twice before our `paused` lands.
static go_t answer(uint32_t key, uint64_t played)
{
    switch (key)
    {
        case OS64_MEDIA_KEY_NEXT:     return GO_NEXT;
        case OS64_MEDIA_KEY_PREVIOUS: return played < PREVIOUS_GOES_BACK_FRAMES ? GO_PREVIOUS : GO_RESTART;
        case OS64_MEDIA_KEY_STOP:     return GO_STOP;
        default:                      return GO_ON;
    }
}

// Paused: the stream stops at once, and nothing is written until Play (or
// a key that moves, which plays on from where it lands, or Ctrl+C). Leaving
// to move or to stop, the paused sound is flushed BEFORE the resume: a
// resume wakes the sink at once, and it would play what was queued.
static go_t paused(player_t *p, uint64_t played)
{
    control(p, "pause");
    say(p, "paused");
    go_t go = GO_ON;
    for (;;)
    {
        uint32_t key;
        if (gStop)
            break;
        if (!next_press(p, OS64_WAIT_FOREVER, &key))
            continue;   // a signal (gStop says whose) or a sibling's close
        if (key == OS64_MEDIA_KEY_PLAY)
            break;
        if (key == OS64_MEDIA_KEY_PAUSE)
            continue;   // already paused
        go = answer(key, played);
        break;
    }
    if (go != GO_ON || gStop)
        control(p, "flush");   // the paused song never sounds again
    control(p, "resume");
    if (go != GO_STOP && !gStop)
        say(p, "playing");
    return go;
}

// Look for a press without waiting, between two pieces of sound. A key
// that moves flushes what is queued: the next sound now, not in a fifth of
// a second (a pause that ends in a move has flushed already).
static go_t keys_now(player_t *p, uint64_t played)
{
    uint32_t key;
    while (next_press(p, 0, &key))
    {
        if (key == OS64_MEDIA_KEY_PAUSE)
            return paused(p, played);
        go_t go = answer(key, played);
        if (go != GO_ON)
        {
            control(p, "flush");
            return go;
        }
    }
    return GO_ON;
}

// Hand `frames` stereo frames to the device. False when the playing must
// end: Ctrl+C, or the device failed.
static bool put(player_t *p, const int16_t *pcm, uint32_t frames)
{
    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t left = (size_t)frames * OS64_AUDIO_FRAME_BYTES;
    while (left > 0)
    {
        if (gStop)
            return false;
        int64_t n = os64_write(p->audio, bytes, left);
        if (n == OS64_INTERRUPTED || n == OS64_AUDIO_FLUSHED)
            continue;   // a signal: gStop says whether it was ours. A flush from elsewhere: play on
        if (n < 0)
        {
            os64_printf("play: the device stopped taking sound (%ld); cat /sys/audio says why\n", n);
            p->exit_code = 3;
            return false;
        }
        bytes += n;
        left -= (size_t)n;
    }
    return true;
}

// "3:25.4", or "unknown length".
static void say_length(int64_t frames, char *out, size_t cap)
{
    if (frames < 0)
    {
        os64_snprintf(out, cap, "unknown length");
        return;
    }
    int64_t tenths = frames / (OS64_AUDIO_RATE / 10);
    os64_snprintf(out, cap, "%ld:%02ld.%ld", tenths / 600, tenths / 10 % 60, tenths % 10);
}

// Play one file until it ends or a key moves away from it.
static go_t play_file(player_t *p, const char *path, bool verbose)
{
    int64_t file = os64_open(path, "r");
    if (file < 0)
    {
        os64_printf("play: %s: cannot open\n", path);
        p->exit_code = 2;
        return GO_SKIP;
    }
    os64_media_source_t source;
    os64_media_file_source((int32_t)file, &source);
    os64_media_t *media = NULL;
    os64_media_info_t info;
    int opened = os64_media_open(&source, &media, &info);
    if (opened != 0)
    {
        os64_printf("play: %s: %s\n", path, os64_media_strerror(opened));
        os64_close((int32_t)file);
        p->exit_code = 2;
        return GO_SKIP;
    }
    if (verbose)
    {
        char length[32];
        say_length(info.frames, length, sizeof(length));
        os64_printf("play: %s: %s (%s), %u Hz, %s, %s\n", path, info.format, info.codec, info.rate,
                    info.channels == 1 ? "mono" : info.channels == 2 ? "stereo" : "more than two channels",
                    length);
    }

    static int16_t pcm[CHUNK_FRAMES * 2];
    uint64_t played = 0;
    go_t go = GO_NEXT;   // the file's end is a Next
    for (;;)
    {
        go_t asked = keys_now(p, played);
        if (asked != GO_ON)
        {
            go = asked;
            break;
        }
        int64_t n = os64_media_decode(media, pcm, CHUNK_FRAMES);
        if (n == 0)
            break;
        if (n < 0)
        {
            const char *why = os64_media_error_text(media);
            os64_printf("play: %s: %s%s%s\n", path, os64_media_strerror((int)n), why[0] ? ": " : "", why);
            p->exit_code = 2;
            break;
        }
        if (!put(p, pcm, (uint32_t)n))
        {
            go = GO_STOP;
            break;
        }
        played += (uint64_t)n;
    }
    os64_media_close(media);
    os64_close((int32_t)file);
    return go;
}

int main(int argc, char **argv)
{
    bool verbose = false;
    int first = 1;
    if (argc > 1 && os64_streq(argv[1], "-v"))
    {
        verbose = true;
        first = 2;
    }
    if (first >= argc)
    {
        os64_printf("usage: play [-v] file...\n");
        return 1;
    }

    player_t p = { .ctl = -1, .keys = -1, .exit_code = 0 };
    int64_t audio = os64_open("/dev/audio", "w");
    if (audio < 0)
    {
        os64_printf(audio == OS64_AUDIO_BUSY ? "play: something else is playing\n"
                                             : "play: /dev/audio would not open\n");
        return 3;
    }
    p.audio = (int32_t)audio;
    int64_t ctl = os64_open("/dev/audioctl", "w");
    p.ctl = ctl >= 0 ? (int32_t)ctl : -1;
    int64_t keys = os64_open("/dev/mediakeys", "u");
    p.keys = keys >= 0 ? (int32_t)keys : -1;
    os64_signal_set_handler(OS64_SIGINT, on_interrupt);
    say(&p, "playing");

    int at = first;
    bool back = false;   // the last move was Previous
    while (at < argc && !gStop)
    {
        go_t go = play_file(&p, argv[at], verbose);
        if (go == GO_STOP || p.exit_code == 3)
            break;
        if (go == GO_SKIP)   // a file that will not play: on the way we were going, forward from the first
            go = back && at > first ? GO_PREVIOUS : GO_NEXT;
        back = go == GO_PREVIOUS;
        if (go == GO_NEXT)
            at++;
        else if (go == GO_PREVIOUS && at > first)
            at--;
        // GO_RESTART, and GO_PREVIOUS at the first file: this one again.
    }

    if (gStop)
    {
        control(&p, "flush");   // silence now, not in a fifth of a second
        p.exit_code = 130;
    }
    if (p.keys >= 0)
        os64_close(p.keys);
    if (p.ctl >= 0)
        os64_close(p.ctl);
    os64_close(p.audio);
    return p.exit_code;
}
