// strip.c: a <video controls>' strip and its buffering mark (strip.h).

#include "strip.h"

void yonder_strip_init(yonder_strip_t *s)
{
    *s = (yonder_strip_t){.state = YONDER_STRIP_HIDDEN, .mark = YONDER_MARK_NONE};
}

static bool pinned(const yonder_strip_t *s)
{
    return s->paused || s->over_strip || s->focused || s->dragging;
}

void yonder_strip_moved(yonder_strip_t *s, uint64_t now)
{
    s->deadline = now + YONDER_STRIP_IDLE_MS;
    if (s->state == YONDER_STRIP_HIDDEN)
        s->state = YONDER_STRIP_TIMED;
}

bool yonder_strip_settle(yonder_strip_t *s, uint64_t now)
{
    bool was_drawn = yonder_strip_drawn(s);
    yonder_mark_t was_mark = s->mark;
    if (pinned(s)) {
        s->state = YONDER_STRIP_PINNED;
    } else if (s->state != YONDER_STRIP_HIDDEN) {
        // PINNED lets go into TIMED from the last move (a deadline already
        // past hides it at once); TIMED hides at its deadline or when the
        // pointer leaves the box, but never while the player buffers.
        s->state = YONDER_STRIP_TIMED;
        if (!s->buffering && (!s->over_box || now >= s->deadline))
            s->state = YONDER_STRIP_HIDDEN;
    }
    if (!s->buffering) {
        s->mark = YONDER_MARK_NONE;
    } else if (s->mark == YONDER_MARK_NONE) {
        s->mark = YONDER_MARK_PENDING;
        s->mark_due = now + YONDER_STRIP_MARK_MS;
    }
    if (s->mark == YONDER_MARK_PENDING && now >= s->mark_due)
        s->mark = YONDER_MARK_SHOWN;
    return was_drawn != yonder_strip_drawn(s) || (was_mark == YONDER_MARK_SHOWN) != (s->mark == YONDER_MARK_SHOWN);
}

uint64_t yonder_strip_next(const yonder_strip_t *s)
{
    uint64_t next = UINT64_MAX;
    if (s->state == YONDER_STRIP_TIMED && !s->buffering && s->over_box)
        next = s->deadline;
    if (s->mark == YONDER_MARK_PENDING && s->mark_due < next)
        next = s->mark_due;
    return next;
}

bool yonder_strip_drawn(const yonder_strip_t *s)
{
    return s->state != YONDER_STRIP_HIDDEN;
}
