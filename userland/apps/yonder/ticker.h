#ifndef YONDER_TICKER_H
#define YONDER_TICKER_H

// The window's clock (YONDER.md § Y5b): a thread that holds one deadline
// and rings the window's doorbell when it passes. The window's loop waits
// for events with no timeout, so moving pictures, and a slow page's
// deferred layout, need somebody else to wake it; the ticker does that and
// nothing more.

#include <stdbool.h>
#include <stdint.h>

#define YONDER_NEVER UINT64_MAX

typedef struct yonder_ticker yonder_ticker_t;

// Milliseconds on the monotonic clock: the deadlines' unit.
uint64_t yonder_now_ms(void);

// A sleeping ticker for `window`, ringing `bell`. NULL when it could not be
// started; the window then has no moving pictures.
yonder_ticker_t *yonder_ticker_start(int64_t window, uint32_t bell);
// The next deadline, or YONDER_NEVER for none. A deadline rings once and
// is forgotten; the window hands over the next one after it has looked.
void yonder_ticker_set(yonder_ticker_t *t, uint64_t due_ms);
// What the thread did since the counts last restarted (the paint counts
// read them): its rings, its looks at the deadline — each ring, each wake,
// each timeout — the most a ring came after its deadline (or after it was
// handed over, for one handed over already past), and the most a sleep
// with a timeout lasted past it, in milliseconds.
typedef struct {
    uint64_t rings, wakes, rang_late_most, overslept_most;
} yonder_ticker_counts_t;
// The counts so far; all zero for a NULL ticker.
yonder_ticker_counts_t yonder_ticker_counts(yonder_ticker_t *t);
// Starts the counts again from zero. NULL is accepted.
void yonder_ticker_counts_restart(yonder_ticker_t *t);
// Stops and joins the thread and frees the ticker. NULL is accepted.
void yonder_ticker_stop(yonder_ticker_t *t);

#endif
