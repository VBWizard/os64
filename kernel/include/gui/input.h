#ifndef GUI_INPUT_H
#define GUI_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#include "os64/gui.h"   // the ring-3 names these constants must keep matching

// Unified input event queue — layer 2 of the GUI.
//
// One ring carries keyboard AND mouse events in arrival order, so the
// compositor sees a single interleaved timeline (a click can't jump ahead of
// the keystroke that preceded it). Producers are the PS/2 IRQ handlers;
// the only consumer is the compositor's frame loop.
//
// HARD RULE for producers: IRQ handlers ONLY enqueue. They must never wake
// threads (scheduler_wake_isleep_task -> scheduler_trigger does sti/hlt,
// which is fatal in IRQ context). The compositor polls each frame instead.

typedef enum input_event_type
{
    INPUT_EVENT_KEY_DOWN = 1,
    INPUT_EVENT_KEY_UP,
    INPUT_EVENT_MOUSE_MOVE,
    INPUT_EVENT_MOUSE_BUTTON_DOWN,
    INPUT_EVENT_MOUSE_BUTTON_UP,
    // Not an INPUT event by origin — the window system synthesizes this one
    // and delivers it straight to a window's queue when its content area
    // changes size. It rides this ring's TYPE space (never the ring itself)
    // because an app already has exactly one place it learns things: its
    // event queue. Geometry news on its own channel would mean every app
    // grows a second thing to poll. X11 made the same call — ConfigureNotify
    // is an ordinary event, not a signal — and it is why an event loop is a
    // loop instead of a switchboard.
    INPUT_EVENT_WINDOW_RESIZE,
} input_event_type_t;

// Mouse button bit positions (in `buttons`, and named in `button` for the
// BUTTON_DOWN/UP events). These numbers are ABI — they ride out to ring 3 in
// every mouse event — so os64/gui.h publishes the same three under their
// public names and the static-asserts below stop the two copies from ever
// drifting apart (the log.c/klog_format.h precedent: a mismatch stops the
// build instead of lying to a client).
#define INPUT_MOUSE_BUTTON_LEFT   0
#define INPUT_MOUSE_BUTTON_RIGHT  1
#define INPUT_MOUSE_BUTTON_MIDDLE 2

_Static_assert(INPUT_MOUSE_BUTTON_LEFT   == OS64_GUI_MOUSE_LEFT,   "mouse button ABI: left");
_Static_assert(INPUT_MOUSE_BUTTON_RIGHT  == OS64_GUI_MOUSE_RIGHT,  "mouse button ABI: right");
_Static_assert(INPUT_MOUSE_BUTTON_MIDDLE == OS64_GUI_MOUSE_MIDDLE, "mouse button ABI: middle");

typedef struct input_event
{
    uint8_t type;   // input_event_type_t
    union {
        struct {
            char    ascii;      // 0 when the key has no glyph (modifiers etc.)
            uint8_t scancode;   // PS/2 set-1 make code
            uint8_t modifiers;  // keyboard_modifiers_t bitmask at event time
        } key;
        struct {
            int32_t x, y;       // cursor position, screen coords (filled by
                                // the mouse driver's position tracking)
            int16_t dx, dy;     // raw motion delta this packet
            uint8_t buttons;    // current button state bitmask
            uint8_t button;     // for BUTTON_DOWN/UP: which button changed
            uint8_t modifiers;  // keyboard_modifiers_t bitmask at event time
                                // (2026-08-19) — the pointer's view of the
                                // keyboard, so a Ctrl+Alt drag can be
                                // recognized from a mouse packet alone
                                // instead of the compositor keeping its own
                                // shadow copy of modifier state and drifting
                                // out of sync with it after a VT switch.
        } mouse;
        struct {
            int32_t w, h;       // the NEW content size, in pixels
        } resize;
    };
    uint64_t tick;  // kTicksSinceStart at enqueue, for input latency debugging
} input_event_t;

// Called by gui_start(). Until this runs, inject calls are cheap no-ops —
// the OS without the GUI has no input consumer, so queueing would be waste.
void input_init(void);

// Producer side (IRQ context safe: irqsave spinlock, enqueue only).
void input_inject_key(char ascii, uint8_t scancode, uint8_t modifiers, bool pressed);
void input_inject_mouse(int16_t dx, int16_t dy, uint8_t buttons);

// Consumer side (compositor only). Returns false when the queue is empty.
bool input_pop(input_event_t *out);

// Lock-free peek: "is anything queued?" — a WAKE-UP HINT for the
// compositor's halt loop, nothing more. A racing producer can flip the
// answer a moment later; the caller must tolerate both a spurious yes and
// a just-missed no (the next timer interrupt bounds the miss).
bool input_pending(void);

#endif // GUI_INPUT_H
