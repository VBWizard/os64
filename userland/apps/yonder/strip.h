#ifndef YONDER_STRIP_H
#define YONDER_STRIP_H

// strip.h: whether a <video controls>' strip is drawn over its picture, and
// whether the buffering mark is (VIDEO.md § 8, V.6.3; the table is
// VIDEO_V6_DESIGN.md § 2 with Fable's answers). Pure, so the host drives it
// beside a model (tools/test_strip_host.sh) and its mutants break each rule
// (tools/test_strip_mutants.py); the window (yonder.c) sets the inputs, tells
// the events, settles it at each turn, and arms one timer for `next`.
//
// THREE STATES:
//   PINNED  drawn, takes clicks, no timer: the element is paused or ended
//           (an autoplaying one never is: it has been asked to play), the
//           pointer is over the strip, a part of it has the keyboard's
//           focus, or a drag on its bar or volume is under way (one that
//           leaves the box stays pinned until the button is released).
//   TIMED   drawn, takes clicks: until the last move over the box + 3 s,
//           or the pointer leaving the box. Buffering holds the deadline:
//           it does not pass while the player buffers (leaving still hides).
//   HIDDEN  not drawn, takes no clicks (a click on the box is a click on
//           the picture). Buffering never shows it: the mark speaks.
// A pointer move over the box, a click on the picture and a key the
// element handles are each a move: HIDDEN to TIMED, the deadline moved.
//
// THE MARK: NONE, PENDING when buffering starts, SHOWN half a second later
// (a seek that lands sooner flashes nothing), NONE when buffering ends.

#include <stdbool.h>
#include <stdint.h>

#define YONDER_STRIP_IDLE_MS 3000u   // a moving pointer keeps the strip this long after its last move
#define YONDER_STRIP_MARK_MS 500u    // buffering this long shows the mark

typedef enum { YONDER_STRIP_HIDDEN, YONDER_STRIP_TIMED, YONDER_STRIP_PINNED } yonder_strip_state_t;
typedef enum { YONDER_MARK_NONE, YONDER_MARK_PENDING, YONDER_MARK_SHOWN } yonder_mark_t;

typedef struct {
    // The inputs, as they stand: the window sets them before each settle.
    bool paused;            // the element is paused or ended
    bool over_box;          // the pointer is over the video's box
    bool over_strip;        // the pointer is over the strip (and the strip is drawn)
    bool focused;           // a part of the strip has the keyboard's focus
    bool dragging;          // a drag on the bar or the volume is under way
    bool buffering;         // the player buffers
    // The machine's own.
    yonder_strip_state_t state;
    uint64_t deadline;      // TIMED's, in the window's milliseconds
    yonder_mark_t mark;
    uint64_t mark_due;      // PENDING's
} yonder_strip_t;

// A strip made (an element given `controls`, or given it again after it was
// taken away): HIDDEN until its first settle, with no history.
void yonder_strip_init(yonder_strip_t *s);
// A move at `now`: the pointer over the box, a click on the picture, or a
// key the element handled.
void yonder_strip_moved(yonder_strip_t *s, uint64_t now);
// The rules applied at `now` to the inputs as they stand. True when what is
// drawn changed (the strip shown or hidden, the mark shown or gone).
bool yonder_strip_settle(yonder_strip_t *s, uint64_t now);
// The next time a settle can change anything with no new input: TIMED's
// deadline (none while buffering) or PENDING's due; UINT64_MAX for never.
uint64_t yonder_strip_next(const yonder_strip_t *s);
// Whether the strip is drawn and takes clicks.
bool yonder_strip_drawn(const yonder_strip_t *s);

#endif
