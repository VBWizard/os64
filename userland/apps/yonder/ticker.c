// ticker.c — the window's clock (ticker.h).

#include "ticker.h"
#include "os64/gui.h"
#include "os64/io.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/thread.h"

// The deadline and the stop are shared; the pipe is how the window wakes
// the thread early, when a deadline moves nearer or it is time to stop.
struct yonder_ticker {
    os64_lock_t lock;
    uint64_t due;
    bool stopping;
    int32_t wake[2];
    int64_t thread;
    int64_t window;
    uint32_t bell;
};

uint64_t yonder_now_ms(void)
{
    os64_ticks_t t;
    if (os64_ticks(&t) < 0 || t.per_second == 0)
        return 0;
    return t.ticks * 1000 / t.per_second;
}

static int64_t tick(void *arg)
{
    yonder_ticker_t *t = arg;
    for (;;) {
        os64_lock_acquire(&t->lock);
        bool stopping = t->stopping;
        uint64_t due = t->due;
        os64_lock_release(&t->lock);
        if (stopping)
            return 0;
        uint64_t now = yonder_now_ms();
        if (due != YONDER_NEVER && due <= now) {
            // Forgotten before the ring, unless the window already moved it:
            // what it sets after looking is the next deadline, and must stand.
            os64_lock_acquire(&t->lock);
            if (t->due == due)
                t->due = YONDER_NEVER;
            os64_lock_release(&t->lock);
            (void)os64_gui_event_ring(t->window, t->bell);
            continue;
        }
        // Every byte waiting says the same thing: look again.
        char bytes[64];
        (void)os64_read_for(t->wake[0], bytes, sizeof(bytes),
                            due == YONDER_NEVER ? OS64_WAIT_FOREVER : due - now);
    }
}

yonder_ticker_t *yonder_ticker_start(int64_t window, uint32_t bell)
{
    yonder_ticker_t *t = os64_calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;
    t->due = YONDER_NEVER;
    t->window = window;
    t->bell = bell;
    if (os64_pipe(t->wake) < 0) {
        os64_free(t);
        return NULL;
    }
    t->thread = os64_thread(tick, t);
    if (t->thread < 0) {
        os64_close(t->wake[0]);
        os64_close(t->wake[1]);
        os64_free(t);
        return NULL;
    }
    return t;
}

static void wake(yonder_ticker_t *t)
{
    const char byte = 1;
    (void)os64_write(t->wake[1], &byte, 1);
}

void yonder_ticker_set(yonder_ticker_t *t, uint64_t due_ms)
{
    if (t == NULL)
        return;
    os64_lock_acquire(&t->lock);
    uint64_t was = t->due;
    t->due = due_ms;
    os64_lock_release(&t->lock);
    // A later deadline needs no wake: the thread wakes at the earlier one,
    // finds this, and sleeps again.
    if (due_ms < was)
        wake(t);
}

void yonder_ticker_stop(yonder_ticker_t *t)
{
    if (t == NULL)
        return;
    os64_lock_acquire(&t->lock);
    t->stopping = true;
    os64_lock_release(&t->lock);
    wake(t);
    (void)os64_thread_join((int32_t)t->thread, NULL);
    os64_close(t->wake[0]);
    os64_close(t->wake[1]);
    os64_free(t);
}
