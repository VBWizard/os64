// play — play a WAV file through /dev/audio (os64/audio.h).
//
//     play song.wav            play it; Ctrl+C stops it at once
//     play -v song.wav         and say what the file is
//
// The file: RIFF/WAVE, PCM, 16 bits, one or two channels, 4 to 384 kHz. One
// channel is doubled. A rate that is not the device's goes through
// libsound's resampler (os64/resample.h). The ordinary blocking write is
// the clock: it returns when the device has taken the sound, so this
// program runs for the length of the song with no timing of its own.
//
// It does not wait for the last of the sound before exiting. A close leaves
// what is queued to play out (a fifth of a second at most), and a `play`
// started within that follows it without a cut.
//
// Exit codes: 0 played; 1 usage; 2 the file would not open or is not a WAV
// this plays; 3 the device refused or failed, or the resampler could not be
// made or stopped; 130 stopped by Ctrl+C.

#include "os64/os64.h"
#include "os64/audio.h"
#include "os64/resample.h"
#include "os64/signal.h"

#define CHUNK_FRAMES 4096u

static volatile int gStop;
static void on_interrupt(int signo) { (void)signo; gStop = 1; }

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }

// Read exactly `want` bytes, or fewer at the end of the file.
static size_t read_fully(int32_t h, void *buf, size_t want)
{
    size_t have = 0;
    while (have < want)
    {
        int64_t n = os64_read(h, (uint8_t *)buf + have, want - have);
        if (n <= 0)
            break;
        have += (size_t)n;
    }
    return have;
}

// 64 bits wide: a chunk's size plus its pad byte may not fit in 32.
static bool skip(int32_t h, uint64_t bytes)
{
    static uint8_t sink[512];
    while (bytes > 0)
    {
        size_t piece = bytes < sizeof(sink) ? bytes : sizeof(sink);
        if (read_fully(h, sink, piece) != piece)
            return false;
        bytes -= piece;
    }
    return true;
}

typedef struct
{
    uint32_t channels, rate, data_bytes;
} wav_t;

// Walk the chunks to "data", having met "fmt " on the way. Leaves the file
// at the first sample. Returns why not, or NULL.
static const char *wav_open(int32_t h, wav_t *w)
{
    uint8_t head[12];
    if (read_fully(h, head, 12) != 12 || os64_memcmp(head, "RIFF", 4) != 0 ||
        os64_memcmp(head + 8, "WAVE", 4) != 0)
        return "not a RIFF/WAVE file";

    bool have_format = false;
    for (;;)
    {
        uint8_t chunk[8];
        if (read_fully(h, chunk, 8) != 8)
            return have_format ? "no sound in it (no data chunk)" : "no format chunk";
        uint32_t size = le32(chunk + 4);
        if (os64_memcmp(chunk, "fmt ", 4) == 0)
        {
            uint8_t f[40];
            if (size < 16)
                return "a format chunk too short to be one";
            uint32_t take = size < sizeof(f) ? size : (uint32_t)sizeof(f);
            if (read_fully(h, f, take) != take)
                return "cut short inside its format chunk";
            uint32_t tag = le16(f);
            // 0xFFFE is "extensible": the real tag is the first two bytes
            // of a GUID at offset 24.
            if (tag == 0xFFFE && take >= 26)
                tag = le16(f + 24);
            if (tag != 1)
                return "not PCM (compressed or floating point)";
            w->channels = le16(f + 2);
            w->rate = le32(f + 4);
            if (le16(f + 14) != 16)
                return "not 16 bits a sample";
            if (w->channels != 1 && w->channels != 2)
                return "not one or two channels";
            if (w->rate < 4000 || w->rate > 384000)
                return "a sample rate nobody records at";
            have_format = true;
            if (!skip(h, (uint64_t)size - take + (size & 1)))
                return "cut short after its format chunk";
        }
        else if (os64_memcmp(chunk, "data", 4) == 0)
        {
            if (!have_format)
                return "its sound comes before its format";
            w->data_bytes = size;
            return NULL;
        }
        else if (!skip(h, (uint64_t)size + (size & 1)))   // chunks are padded to an even length
            return "cut short inside a chunk";
    }
}

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
        os64_printf("usage: play [-v] file.wav\n");
        return 1;
    }

    int64_t file = os64_open(path, "r");
    if (file < 0)
    {
        os64_printf("play: %s: cannot open\n", path);
        return 2;
    }
    wav_t wav = { 0, 0, 0 };
    const char *why = wav_open((int32_t)file, &wav);
    if (why != NULL)
    {
        os64_printf("play: %s: %s\n", path, why);
        return 2;
    }
    if (verbose)
        os64_printf("play: %s: %u Hz, %s, %u.%u s\n", path, wav.rate,
                    wav.channels == 2 ? "stereo" : "mono",
                    wav.data_bytes / (wav.rate * wav.channels * 2),
                    wav.data_bytes / (wav.rate * wav.channels * 2 / 10) % 10);

    os64_resampler_t *resampler = NULL;
    if (wav.rate != OS64_AUDIO_RATE)
    {
        resampler = os64_resampler_create(OS64_AUDIO_CHANNELS, wav.rate, OS64_AUDIO_RATE);
        if (resampler == NULL)
        {
            os64_printf("play: no memory for the resampler\n");
            return 3;
        }
    }

    int64_t audio = os64_open("/dev/audio", "w");
    if (audio < 0)
    {
        os64_printf(audio == OS64_AUDIO_BUSY ? "play: something else is playing\n"
                                             : "play: /dev/audio would not open\n");
        return 3;
    }
    os64_signal_set_handler(OS64_SIGINT, on_interrupt);

    static int16_t in[CHUNK_FRAMES * 2];
    // Room for one chunk at the largest ratio this accepts (4 kHz up to 48).
    static int16_t out[CHUNK_FRAMES * 2 * 13];
    int exit_code = 0;
    uint32_t left = wav.data_bytes / (wav.channels * 2);   // frames
    uint32_t tail = resampler != NULL ? os64_resampler_latency(resampler) : 0;
    bool playing = true;

    while (playing && (left > 0 || tail > 0))
    {
        uint32_t frames;
        if (left > 0)
        {
            uint32_t want = left < CHUNK_FRAMES ? left : CHUNK_FRAMES;
            size_t got = read_fully((int32_t)file, in, (size_t)want * wav.channels * 2);
            frames = (uint32_t)(got / (wav.channels * 2));
            left = frames == want ? left - frames : 0;   // a file shorter than it says ends here
            if (wav.channels == 1)
                for (uint32_t i = frames; i-- > 0;)      // backward: in place
                    in[i * 2] = in[i * 2 + 1] = in[i];
        }
        else
        {
            // The song is in; what the resampler still holds is drawn out
            // with as much silence.
            frames = tail < CHUNK_FRAMES ? tail : CHUNK_FRAMES;
            os64_memset(in, 0, (size_t)frames * 4);
            tail -= frames;
        }

        if (resampler == NULL)
        {
            playing = put((int32_t)audio, in, frames, &exit_code);
            continue;
        }
        uint32_t done = 0;
        while (playing && done < frames)
        {
            uint32_t take = frames - done, made = (uint32_t)(sizeof(out) / 4);
            if (os64_resampler_run(resampler, in + done * 2, &take, out, &made) != 0 ||
                (take == 0 && made == 0))
            {
                os64_printf("play: the resampler stopped\n");
                exit_code = 3;
                playing = false;
                break;
            }
            done += take;
            playing = put((int32_t)audio, out, made, &exit_code);
        }
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
    os64_close((int32_t)file);
    os64_resampler_destroy(resampler);
    return exit_code;
}
