// play — play a sound file through /dev/audio (os64/audio.h).
//
//     play song.mp3            play it; Ctrl+C stops it at once
//     play -v song.wav         and say what the file is
//
// The file: anything libmedia reads (os64/media.h): MP3, and WAV of 8,
// 16, 24 or 32 bits or 32-bit floating point, a-law or mu-law, at any rate
// and channel count. libmedia hands back the
// device's own format (16-bit stereo at 48 kHz), so this program only
// moves sound. The ordinary blocking write is the clock: it returns when
// the device has taken the sound, so this program runs for the length of
// the song with no timing of its own.
//
// It does not wait for the last of the sound before exiting. A close leaves
// what is queued to play out (a fifth of a second at most), and a `play`
// started within that follows it without a cut.
//
// Exit codes: 0 played; 1 usage; 2 the file would not open, is not sound
// this plays, or is damaged past playing; 3 the device refused or failed;
// 130 stopped by Ctrl+C.

#include "os64/os64.h"
#include "os64/audio.h"
#include "os64/media.h"
#include "os64/signal.h"

#define CHUNK_FRAMES 4096u

static volatile int gStop;
static void on_interrupt(int signo) { (void)signo; gStop = 1; }

// Hand `frames` stereo frames to the device. False when the playing must
// end: Ctrl+C, or the device failed.
static bool put(int32_t audio, const int16_t *pcm, uint32_t frames, int *exit_code)
{
    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t left = (size_t)frames * OS64_AUDIO_FRAME_BYTES;
    while (left > 0)
    {
        if (gStop)
            return false;
        int64_t n = os64_write(audio, bytes, left);
        if (n == OS64_INTERRUPTED || n == OS64_AUDIO_FLUSHED)
            continue;   // a signal: gStop says whether it was ours. A flush from elsewhere: play on
        if (n < 0)
        {
            os64_printf("play: the device stopped taking sound (%ld); cat /sys/audio says why\n", n);
            *exit_code = 3;
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

int main(int argc, char **argv)
{
    bool verbose = false;
    const char *path = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (os64_streq(argv[i], "-v"))
            verbose = true;
        else if (path == NULL)
            path = argv[i];
        else
            path = "";
    }
    if (path == NULL || path[0] == '\0')
    {
        os64_printf("usage: play [-v] file\n");
        return 1;
    }

    int64_t file = os64_open(path, "r");
    if (file < 0)
    {
        os64_printf("play: %s: cannot open\n", path);
        return 2;
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
        return 2;
    }
    if (verbose)
    {
        char length[32];
        say_length(info.frames, length, sizeof(length));
        os64_printf("play: %s: %s (%s), %u Hz, %s, %s\n", path, info.format, info.codec, info.rate,
                    info.channels == 1 ? "mono" : info.channels == 2 ? "stereo" : "more than two channels",
                    length);
    }

    int64_t audio = os64_open("/dev/audio", "w");
    if (audio < 0)
    {
        os64_printf(audio == OS64_AUDIO_BUSY ? "play: something else is playing\n"
                                             : "play: /dev/audio would not open\n");
        os64_media_close(media);
        os64_close((int32_t)file);
        return 3;
    }
    os64_signal_set_handler(OS64_SIGINT, on_interrupt);

    static int16_t pcm[CHUNK_FRAMES * 2];
    int exit_code = 0;
    for (;;)
    {
        int64_t n = os64_media_decode(media, pcm, CHUNK_FRAMES);
        if (n == 0)
            break;
        if (n < 0)
        {
            const char *why = os64_media_error_text(media);
            os64_printf("play: %s: %s%s%s\n", path, os64_media_strerror((int)n), why[0] ? ": " : "", why);
            exit_code = 2;
            break;
        }
        if (!put((int32_t)audio, pcm, (uint32_t)n, &exit_code))
            break;
    }

    if (gStop)
    {
        // Silence now, not in a fifth of a second.
        int64_t ctl = os64_open("/dev/audioctl", "w");
        if (ctl >= 0)
        {
            os64_write((int32_t)ctl, "flush\n", 6);
            os64_close((int32_t)ctl);
        }
        exit_code = 130;
    }
    os64_close((int32_t)audio);
    os64_media_close(media);
    os64_close((int32_t)file);
    return exit_code;
}
