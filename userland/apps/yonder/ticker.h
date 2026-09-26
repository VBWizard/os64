#ifndef YONDER_TICKER_H
#define YONDER_TICKER_H

// The window's clock (YONDER.md § Y5b): a thread that holds one deadline
// and rings the window's doorbell when it passes. The window's loop waits
// for events with no timeout, so moving pictures need somebody else to
// wake it; the ticker does that and nothing more.

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
// Stops and joins the thread and frees the ticker. NULL is accepted.
void yonder_ticker_stop(yonder_ticker_t *t);

#endif
