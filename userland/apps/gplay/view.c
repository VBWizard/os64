// view.c — gplay's arithmetic that needs no machine (view.h).

#include "view.h"

#define RATE 48000

gplay_rect_t gplay_letterbox(uint32_t src_w, uint32_t src_h, uint32_t box_w, uint32_t box_h)
{
    gplay_rect_t r = {0, 0, 0, 0};
    if (src_w == 0 || src_h == 0 || box_w == 0 || box_h == 0)
        return r;
    // Fill the width when the picture is at least as wide, in shape, as
    // the box (src_w / src_h >= box_w / box_h), else fill the height.
    if ((uint64_t)src_w * box_h >= (uint64_t)box_w * src_h) {
        r.w = box_w;
        r.h = (uint32_t)(((uint64_t)box_w * src_h + src_w / 2) / src_w);
    } else {
        r.h = box_h;
        r.w = (uint32_t)(((uint64_t)box_h * src_w + src_h / 2) / src_h);
    }
    if (r.w == 0)
        r.w = 1;
    if (r.h == 0)
        r.h = 1;
    if (r.w > box_w)
        r.w = box_w;
    if (r.h > box_h)
        r.h = box_h;
    r.x = (int32_t)((box_w - r.w) / 2);
    r.y = (int32_t)((box_h - r.h) / 2);
    return r;
}

void gplay_first_size(uint32_t pic_w, uint32_t pic_h, uint32_t screen_w, uint32_t screen_h, uint32_t margin,
                      uint32_t *w, uint32_t *h)
{
    uint32_t room_w = screen_w > 2 * margin ? screen_w - 2 * margin : 1;
    uint32_t room_h = screen_h > 2 * margin ? screen_h - 2 * margin : 1;
    if (pic_w <= room_w && pic_h <= room_h) {
        *w = pic_w;
        *h = pic_h;
        return;
    }
    gplay_rect_t r = gplay_letterbox(pic_w, pic_h, room_w, room_h);
    *w = r.w;
    *h = r.h;
}

// Two digits of `v` (0..99) at `out`.
static void two(char *out, int64_t v)
{
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
}

size_t gplay_time_text(int64_t frames, char *out, size_t cap)
{
    if (cap == 0)
        return 0;
    int64_t s = frames > 0 ? frames / RATE : 0;
    int64_t hours = s / 3600, minutes = (s / 60) % 60, seconds = s % 60;
    char buf[32];
    size_t n = 0;
    int64_t lead = hours > 0 ? hours : minutes;
    char digits[20];
    size_t d = 0;
    do {
        digits[d++] = (char)('0' + lead % 10);
        lead /= 10;
    } while (lead > 0);
    while (d > 0)
        buf[n++] = digits[--d];
    if (hours > 0) {
        buf[n++] = ':';
        two(buf + n, minutes);
        n += 2;
    }
    buf[n++] = ':';
    two(buf + n, seconds);
    n += 2;
    size_t len = n < cap - 1 ? n : cap - 1;
    for (size_t i = 0; i < len; i++)
        out[i] = buf[i];
    out[len] = '\0';
    return len;
}

gplay_key_t gplay_key(char ascii, uint8_t scancode, bool shift)
{
    switch (ascii) {
    case ' ':
        return GPLAY_KEY_PAUSE;
    case 'm':
    case 'M':
        return GPLAY_KEY_MUTE;
    case 'i':
    case 'I':
        return GPLAY_KEY_READOUT;
    case 'q':
    case 'Q':
    case 27:
        return GPLAY_KEY_QUIT;
    case '[':
        return GPLAY_KEY_SOUND_EARLIER;
    case ']':
        return GPLAY_KEY_SOUND_LATER;
    default:
        break;
    }
    switch (scancode) {
    case 0x01:
        return GPLAY_KEY_QUIT;
    case 0x4B:
        return shift ? GPLAY_KEY_BACK_FAR : GPLAY_KEY_BACK;
    case 0x4D:
        return shift ? GPLAY_KEY_FORWARD_FAR : GPLAY_KEY_FORWARD;
    case 0x48:
        return GPLAY_KEY_LOUDER;
    case 0x50:
        return GPLAY_KEY_SOFTER;
    default:
        return GPLAY_KEY_NONE;
    }
}

int64_t gplay_seek_target(int64_t base, int64_t delta, int64_t length)
{
    int64_t t = base + delta;
    if (length >= 0 && t > length - 1)
        t = length - 1;
    if (t < 0)
        t = 0;
    return t;
}
