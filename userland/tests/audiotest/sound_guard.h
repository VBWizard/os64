#ifndef SOUND_GUARD_H
#define SOUND_GUARD_H
// sound_guard.h — for the fixtures that play test signals through
// /dev/audio and set /dev/volume to compare what the tap hears exactly
// (audiotest, playtest). They run only when NOTHING CAN BE HEARD: on the
// null sink, which takes sound at an output's pace and discards it. A real
// sink is a person's headphones, and with absolute volume /dev/volume is
// THEIR volume: a test that set it to 100 and played a tone would play it
// at full volume into somebody's ears and leave them there. So a fixture
// that finds one attached says why and reports SKIP; on the null sink it
// puts the volume back as it found it.

#include "os64/os64.h"

// The attached sink's name from /sys/audio's "sink:" line into `name`.
// True when it is anything but the null sink, or when the line cannot be
// read (an unknown answer is treated as audible).
static bool sound_guard_audible(char *name, size_t cap)
{
    static char text[2048];
    int64_t h = os64_open("/sys/audio", "r");
    int64_t n = h < 0 ? h : os64_read((int32_t)h, text, sizeof(text) - 1);
    if (h >= 0) os64_close((int32_t)h);
    os64_strcopy(name, cap, "an unknown sink");
    if (n <= 0) return true;
    text[n] = 0;
    for (char *line = text; line != NULL && *line;) {
        char *next = os64_strchr(line, '\n');
        if (next != NULL) *next++ = 0;
        if (line[0] == 's' && line[1] == 'i' && line[2] == 'n' && line[3] == 'k' && line[4] == ':' && line[5] == ' ') {
            size_t len = 0;
            while (line[6 + len] && line[6 + len] != ' ') len++;   // the name, before any " FAILED: ..."
            line[6 + len] = 0;
            os64_strcopy(name, cap, line + 6);
            return !os64_streq(line + 6, "null");
        }
        line = next;
    }
    return true;
}

// /dev/volume as a number, or -1 when it cannot be read.
static int sound_guard_volume_get(void)
{
    char text[16];
    int64_t h = os64_open("/dev/volume", "r");
    int64_t n = h < 0 ? h : os64_read((int32_t)h, text, sizeof(text) - 1);
    if (h >= 0) os64_close((int32_t)h);
    if (n <= 0) return -1;
    int v = 0;
    for (int64_t i = 0; i < n && text[i] >= '0' && text[i] <= '9'; i++) v = v * 10 + (text[i] - '0');
    return v <= 100 ? v : -1;
}

static void sound_guard_volume_set(int v)
{
    char text[16];
    int len = os64_snprintf(text, sizeof(text), "%d\n", v);
    int64_t h = os64_open("/dev/volume", "w");
    if (h >= 0) {
        os64_write((int32_t)h, text, (size_t)len);
        os64_close((int32_t)h);
    }
}

#endif
