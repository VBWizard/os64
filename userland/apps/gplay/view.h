#ifndef GPLAY_VIEW_H
#define GPLAY_VIEW_H

// view.h — gplay's arithmetic that needs no machine: where the picture sits
// in the window, how big the window starts, how a time reads, what a key
// asks for, and where a seek lands. Pure, so tools/test_gplay_host.sh runs
// it on the host.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t x, y;
    uint32_t w, h;
} gplay_rect_t;

// The picture (src_w x src_h, its shown shape) inside a box, as large as it
// fits with its shape kept, centred; the rest of the box is black. A side
// of 0 gives an empty rectangle.
gplay_rect_t gplay_letterbox(uint32_t src_w, uint32_t src_h, uint32_t box_w, uint32_t box_h);

// The window's first content size: the picture's shown size, shrunk with its
// shape kept until it fits inside the screen less `margin` on every side.
void gplay_first_size(uint32_t pic_w, uint32_t pic_h, uint32_t screen_w, uint32_t screen_h, uint32_t margin,
                      uint32_t *w, uint32_t *h);

// `frames` (48 kHz) as "m:ss", or "h:mm:ss" from an hour; a time below 0
// reads as 0. Returns the length written, NUL not counted; `cap` of at
// least 16 always holds it.
size_t gplay_time_text(int64_t frames, char *out, size_t cap);

// What a key asks for.
typedef enum {
    GPLAY_KEY_NONE,
    GPLAY_KEY_PAUSE,         // space
    GPLAY_KEY_BACK,          // left: five seconds back; with Shift a minute
    GPLAY_KEY_FORWARD,       // right
    GPLAY_KEY_BACK_FAR,
    GPLAY_KEY_FORWARD_FAR,
    GPLAY_KEY_MUTE,          // m
    GPLAY_KEY_LOUDER,        // up
    GPLAY_KEY_SOFTER,        // down
    GPLAY_KEY_SOUND_LATER,   // ]: the sound later against the picture by 10 ms
    GPLAY_KEY_SOUND_EARLIER, // [
    GPLAY_KEY_READOUT,       // i
    GPLAY_KEY_QUIT,          // q, Escape
} gplay_key_t;

// From a key-down's character, its key's code and Shift. An arrow arrives as
// three key-downs, ESC '[' and a letter (the VT100's bytes, from a PS/2 and
// a USB keyboard alike, each carrying the arrow's own code and modifiers),
// so what has been seen of a burst is kept in `*seq` between calls, zero to
// begin with. The Escape key itself is told from a burst's ESC by its code:
// PS/2 set 1's make code 0x01, or USB's usage 0x29.
gplay_key_t gplay_key(uint8_t *seq, char ascii, uint8_t scancode, bool shift);

// Where a relative seek of `delta` frames lands: from `base` (the pending
// target while a seek is under way, else the clock), held to 0 and, when the
// length is known (`length` >= 0), to one frame before its end.
int64_t gplay_seek_target(int64_t base, int64_t delta, int64_t length);

#endif // GPLAY_VIEW_H
