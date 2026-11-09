// guiwake — does a ring reach a window's waiter that is on its way to the
// park (WAKE_OWED.md)?
//
// One thread waits on its window; another rings it, thousands of times, each
// ring a random fraction of a millisecond after the waiter took the last one
// — so some rings land while the waiter is between registering and parking,
// the window a wake used to be lost in. The ringer writes the time just
// before each ring, and the waiter keeps the worst ring-to-wake latency. A
// lost wake waits for the event wait's quarter-second backstop, so a worst
// case anywhere near 250 ms is the race; the pass bar is 100 ms, far above
// what scheduling alone costs and far below the backstop.
//
// Run in a GUI guest (testrun, or by hand: `guiwake [rounds]`).
#include "os64/os64.h"

#define GUIWAKE_OK   0x60E00000
#define GUIWAKE_FAIL 0x60E00001
#define GUIWAKE_SKIP 0x60E00002

#define ROUNDS_DEFAULT 2000
#define BAR_US         100000      // worse than this is a wake that waited for the backstop
#define SLOW_US        50000       // counted, to say how often it happened
#define JITTER_US      1000        // how far after a take a ring may land
#define STALL_US       3000000     // a ring nobody takes in this long ends the run

typedef struct {
    int64_t win;
    uint32_t rounds;
    int64_t rang_us;               // when the latest ring was sent
    uint32_t sent, taken;          // rings sent, and taken by the waiter
    bool stalled;
} shared_t;

static uint64_t s_rng;

static uint32_t next_random(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)s_rng;
}

// Rings once a take is seen, after a random sub-millisecond spin. Spinning,
// not sleeping: a sleep is at least a tick, and the point is to land inside
// the microseconds between the waiter's registration and its park.
static int64_t ringer(void *arg)
{
    shared_t *s = arg;
    for (uint32_t i = 0; i < s->rounds; i++) {
        int64_t since = os64_micros();
        while (__atomic_load_n(&s->taken, __ATOMIC_ACQUIRE) < i) {
            if (os64_micros() - since > STALL_US) {
                __atomic_store_n(&s->stalled, true, __ATOMIC_RELEASE);
                return -1;
            }
            os64_yield();
        }
        int64_t until = os64_micros() + (int64_t)(next_random() % JITTER_US);
        while (os64_micros() < until)
            ;
        __atomic_store_n(&s->rang_us, os64_micros(), __ATOMIC_RELEASE);
        __atomic_store_n(&s->sent, i + 1, __ATOMIC_RELEASE);
        if (os64_gui_event_ring(s->win, 1) != 0)
            return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t rounds = ROUNDS_DEFAULT;
    if (argc == 2) {
        rounds = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            rounds = rounds * 10 + (uint32_t)(*p - '0');
        if (rounds == 0)
            rounds = ROUNDS_DEFAULT;
    }
    int64_t win = os64_gui_window_create("guiwake", 48, 48, 160, 100, 0);
    if (win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("guiwake: SKIP (requires GUI)\n");
        return GUIWAKE_SKIP;
    }
    if (win <= 0) {
        os64_printf("guiwake: FAIL (no window: %ld)\n", win);
        return GUIWAKE_FAIL;
    }
    // Whatever the window was born with (focus, the first paint) is not a ring.
    os64_gui_event_t ev;
    while (os64_gui_event_poll(win, &ev) == 1)
        ;

    s_rng = (uint64_t)os64_micros() | 1;
    shared_t s = {.win = win, .rounds = rounds};
    int64_t thread = os64_thread(ringer, &s);
    if (thread < 0) {
        os64_printf("guiwake: FAIL (no ringer thread: %ld)\n", thread);
        os64_gui_window_destroy(win);
        return GUIWAKE_FAIL;
    }

    int64_t worst = 0, total = 0;
    uint32_t slow = 0, taken = 0;
    while (taken < rounds && !__atomic_load_n(&s.stalled, __ATOMIC_ACQUIRE)) {
        int64_t rc = os64_gui_event_wait(win, &ev);
        if (rc == OS64_INTERRUPTED)
            continue;
        if (rc != 1)
            break;
        if (ev.type != OS64_GUI_EVENT_DOORBELL ||
            __atomic_load_n(&s.sent, __ATOMIC_ACQUIRE) <= taken)
            continue;
        int64_t late = os64_micros() - __atomic_load_n(&s.rang_us, __ATOMIC_ACQUIRE);
        if (late > worst)
            worst = late;
        total += late;
        slow += late > SLOW_US;
        __atomic_store_n(&s.taken, ++taken, __ATOMIC_RELEASE);
    }

    int64_t result = -1;
    (void)os64_thread_join((int32_t)thread, &result);
    os64_close((int32_t)thread);
    os64_gui_window_destroy(win);

    bool pass = taken == rounds && result == 0 && worst < BAR_US;
    os64_printf("guiwake: %s: %u of %u rings taken, worst %ld.%ld ms, mean %ld us, %u over %d ms\n",
                pass ? "PASS" : "FAIL", taken, rounds, worst / 1000, worst / 100 % 10,
                taken > 0 ? total / taken : 0, slow, SLOW_US / 1000);
    return pass ? GUIWAKE_OK : GUIWAKE_FAIL;
}
