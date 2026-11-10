// belltest — the bell (os64/bell.h, SYSCALL_BELL): a handle whose read
// waits and whose write rings.
//
// Each rule the header states is one step here, through the real syscalls
// with real second threads, and each step has its own exit code:
//
//   1. a ring made BEFORE the wait is kept, and ends the wait at once;
//   2. an unrung wait runs out its patience, all of it, and answers
//      OS64_ERR_TIMEOUT;
//   3. two rings before a wait are one;
//   4. a ring from another thread wakes a PARKED waiter, promptly;
//   5. a second thread waiting on a bell that has a waiter is refused;
//   6. a close under a parked waiter wakes it with 0, promptly;
//   7. a signal the program handles ends a parked wait with
//      OS64_INTERRUPTED, and the handler ran (a child process sends it, so
//      no thread of ours can take the signal instead of the waiter);
//   8. a bell is this program's alone: it is listed in /proc/self/handles
//      as `bell`, a spawned child's list has none, and a spawn that names
//      one as the child's stdin is refused;
//   9. THE RACE: thousands of rings, each a random fraction of a
//      millisecond after the waiter took the last one, so some land between
//      the waiter's registration and its park. A ring lost there waits for
//      the kernel's one-second backstop; the bar is a quarter of that.
//
// `belltest [rounds]` runs it; `belltest child` and `belltest poke <task>`
// are the helpers it spawns.
//
// Exit codes: 0xBE110000 success, 0xBE1100NN the step that failed.

#include "os64/os64.h"
#include "os64/bell.h"
#include "os64/signal.h"

#define BELLTEST_OK 0xBE110000
#define STEP(n)     (0xBE110000 | (n))

// Enough for a thread to be genuinely PARKED before the main thread acts.
#define SETTLE_MS 200

#define ROUNDS_DEFAULT 2000
#define BAR_US         250000      // worse than this is a wake that waited for the backstop
#define SLOW_US        50000       // counted, to say how often it happened
#define JITTER_US      1000        // how far after a take a ring may land
#define STALL_US       5000000     // a ring nobody takes in this long ends the run
#define PROMPT_US      250000      // step 4's bar for one cross-thread wake

static int die(int step, const char *why, int64_t got)
{
    os64_printf("belltest: FAIL step %d: %s (got %ld)\n", step, why, got);
    return (int)STEP(step);
}

// ── Threads the steps start ─────────────────────────────────────────────

static int64_t wait_forever(void *arg)
{
    return os64_bell_wait((os64_bell_t *)arg, OS64_WAIT_FOREVER);
}

// ── Step 7: the handler, and the child that sends the signal ────────────

static volatile int gHandled;

static void on_interrupt(int signo)
{
    gHandled = signo;
}

static uint64_t parse_number(const char *s)
{
    uint64_t n = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        n = n * 10 + (uint64_t)(*s - '0');
    return n;
}

static int poke_main(const char *task_text)
{
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", (unsigned long)parse_number(task_text));
    os64_sleep(SETTLE_MS);              // let the parent park first
    int64_t ctl = os64_open(path, "w");
    if (ctl < 0)
        return 1;
    int64_t wrote = os64_write((int32_t)ctl, "interrupt", 9);
    os64_close((int32_t)ctl);
    return wrote == 9 ? 0 : 2;
}

// ── Step 8: how many bells does this task's handle list show? ───────────

static int bells_listed(void)
{
    static char text[8192];
    int64_t h = os64_open("/proc/self/handles", "r");
    if (h < 0)
        return -1;
    size_t have = 0;
    for (;;) {
        int64_t n = os64_read((int32_t)h, text + have, sizeof(text) - 1 - have);
        if (n <= 0)
            break;
        have += (size_t)n;
    }
    os64_close((int32_t)h);
    text[have] = '\0';
    int bells = 0;
    for (size_t i = 0; i + 5 <= have; i++)
        if (text[i] == '\t' && text[i + 1] == 'b' && text[i + 2] == 'e' &&
            text[i + 3] == 'l' && text[i + 4] == 'l')
            bells++;
    return bells;
}

// ── Step 9: the race ────────────────────────────────────────────────────

typedef struct {
    os64_bell_t bell;
    uint32_t rounds;
    int64_t rang_us;               // when the latest ring was sent
    uint32_t sent, taken;          // rings sent, and taken by the waiter
    bool stalled;
} race_t;

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
    race_t *s = arg;
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
        if (os64_bell_ring(&s->bell) != 0)
            return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && os64_streq(argv[1], "child"))
        return bells_listed() == 0 ? 0 : 1;
    if (argc == 3 && os64_streq(argv[1], "poke"))
        return poke_main(argv[2]);

    uint32_t rounds = argc == 2 ? (uint32_t)parse_number(argv[1]) : 0;
    if (rounds == 0)
        rounds = ROUNDS_DEFAULT;

    os64_bell_t bell = OS64_BELL_NONE;
    if (os64_bell_ring(&bell) >= 0 || os64_bell_wait(&bell, 0) >= 0)
        return die(1, "a bell that is none was rung or waited on", 0);
    int64_t r = os64_bell_make(&bell);
    if (r != 0)
        return die(1, "no bell", r);

    // 1. Ring, then wait: the ring was kept.
    r = os64_bell_ring(&bell);
    if (r != 0)
        return die(1, "a ring was refused", r);
    int64_t before = os64_micros();
    r = os64_bell_wait(&bell, OS64_WAIT_FOREVER);
    if (r != 1 || os64_micros() - before > PROMPT_US)
        return die(1, "a ring made before the wait did not end it at once", r);

    // 2. Unrung: the patience, and all of it. (Rounded UP to the tick, so
    // "all of it" allows nothing short.)
    before = os64_micros();
    r = os64_bell_wait(&bell, 100);
    int64_t took = os64_micros() - before;
    if (r != OS64_ERR_TIMEOUT || took < 100 * 1000 - 10000)
        return die(2, "an unrung wait did not run out its patience", r);
    if (os64_bell_wait(&bell, 0) != OS64_ERR_TIMEOUT)
        return die(2, "a look at an unrung bell did not answer TIMEOUT", 0);

    // 3. Two rings, one wait; the look after it finds nothing.
    os64_bell_ring(&bell);
    os64_bell_ring(&bell);
    r = os64_bell_wait(&bell, 0);
    if (r != 1)
        return die(3, "two rings did not end a wait", r);
    r = os64_bell_wait(&bell, 0);
    if (r != OS64_ERR_TIMEOUT)
        return die(3, "two rings were counted as two", r);

    // 4. A parked waiter, rung from here.
    int64_t waiter = os64_thread(wait_forever, &bell);
    if (waiter < 0)
        return die(4, "no waiter thread", waiter);
    os64_sleep(SETTLE_MS);
    before = os64_micros();
    os64_bell_ring(&bell);
    int64_t answer = -99;
    os64_thread_join((int32_t)waiter, &answer);
    int64_t woke_us = os64_micros() - before;
    os64_close((int32_t)waiter);
    if (answer != 1 || woke_us > PROMPT_US)
        return die(4, "a ring did not wake the parked waiter promptly", answer);

    // 5. One waiter: a second is refused, and the first is none the worse.
    waiter = os64_thread(wait_forever, &bell);
    if (waiter < 0)
        return die(5, "no waiter thread", waiter);
    os64_sleep(SETTLE_MS);
    r = os64_bell_wait(&bell, 0);
    if (r >= 0 || r == OS64_ERR_TIMEOUT || r == OS64_INTERRUPTED)
        return die(5, "a second waiter was not refused", r);
    os64_bell_ring(&bell);
    answer = -99;
    os64_thread_join((int32_t)waiter, &answer);
    os64_close((int32_t)waiter);
    if (answer != 1)
        return die(5, "the first waiter lost its ring to a refused second", answer);

    // 6. Closed under a parked waiter. The waiter was handed its own copy of
    // the bell's handle, since the close below rewrites ours.
    os64_bell_t held = bell;
    waiter = os64_thread(wait_forever, &held);
    if (waiter < 0)
        return die(6, "no waiter thread", waiter);
    os64_sleep(SETTLE_MS);
    before = os64_micros();
    os64_bell_close(&bell);
    answer = -99;
    os64_thread_join((int32_t)waiter, &answer);
    int64_t closed_us = os64_micros() - before;
    os64_close((int32_t)waiter);
    // Promptly: a waiter the close did not wake still finds the bell closed
    // when the kernel's one-second backstop wakes it, and answers 0 too.
    if (answer != 0 || closed_us > PROMPT_US)
        return die(6, "a close under a parked waiter did not wake it with 0, promptly", answer);
    if (bell.handle != -1 || os64_bell_ring(&bell) >= 0)
        return die(6, "a closed bell is still one", bell.handle);

    // 7. A handled signal ends a parked wait.
    if ((r = os64_bell_make(&bell)) != 0)
        return die(7, "no bell", r);
    if (os64_signal_set_handler(OS64_SIGINT, on_interrupt) < 0)
        return die(7, "the SIGINT handler was refused", 0);
    char me[24];
    os64_snprintf(me, sizeof(me), "%lu", (unsigned long)os64_taskid());
    int64_t kid = os64_spawn("/tests/belltest", (char *[]){ "/tests/belltest", "poke", me, 0 });
    if (kid < 0)
        return die(7, "could not spawn the sender", kid);
    r = os64_bell_wait(&bell, 10000);
    if (r != OS64_INTERRUPTED || gHandled != OS64_SIGINT)
        return die(7, "a handled signal did not end the wait with INTERRUPTED", r);
    int32_t code = -1;
    while (os64_wait(kid, &code) == OS64_INTERRUPTED)
        ;
    os64_signal_set_handler(OS64_SIGINT, OS64_SIG_DEFAULT);
    if (code != 0)
        return die(7, "the sender could not write our ctl", code);
    if (os64_bell_wait(&bell, 0) != OS64_ERR_TIMEOUT)
        return die(7, "an interrupted wait left the bell rung", 0);

    // 8. Ours alone.
    int listed = bells_listed();
    if (listed != 1)
        return die(8, "/proc/self/handles does not list exactly one bell", listed);
    kid = os64_spawn("/tests/belltest", (char *[]){ "/tests/belltest", "child", 0 });
    if (kid < 0)
        return die(8, "could not spawn the child", kid);
    code = -1;
    os64_wait(kid, &code);
    if (code != 0)
        return die(8, "a spawned child's handle list has a bell in it", code);
    kid = os64_spawn_redirected("/tests/belltest", (char *[]){ "/tests/belltest", "child", 0 },
                                bell.handle, -1, -1, 0);
    if (kid >= 0) {
        os64_wait(kid, &code);
        return die(8, "a spawn took a bell as the child's stdin", kid);
    }
    os64_bell_close(&bell);

    // 9. The race.
    s_rng = (uint64_t)os64_micros() | 1;
    race_t s = { .bell = OS64_BELL_NONE, .rounds = rounds };
    if ((r = os64_bell_make(&s.bell)) != 0)
        return die(9, "no bell", r);
    int64_t thread = os64_thread(ringer, &s);
    if (thread < 0)
        return die(9, "no ringer thread", thread);
    int64_t worst = 0, total = 0;
    uint32_t slow = 0, taken = 0;
    while (taken < rounds && !__atomic_load_n(&s.stalled, __ATOMIC_ACQUIRE)) {
        r = os64_bell_wait(&s.bell, 2000);
        if (r == OS64_ERR_TIMEOUT || r == OS64_INTERRUPTED)
            continue;
        if (r != 1)
            break;
        if (__atomic_load_n(&s.sent, __ATOMIC_ACQUIRE) <= taken)
            continue;
        int64_t late = os64_micros() - __atomic_load_n(&s.rang_us, __ATOMIC_ACQUIRE);
        if (late > worst)
            worst = late;
        total += late;
        slow += late > SLOW_US;
        __atomic_store_n(&s.taken, ++taken, __ATOMIC_RELEASE);
    }
    int64_t result = -1;
    os64_thread_join((int32_t)thread, &result);
    os64_close((int32_t)thread);
    os64_bell_close(&s.bell);

    bool pass = taken == rounds && result == 0 && worst < BAR_US;
    os64_printf("belltest: %s: steps 1-8 ok (a parked waiter woke in %ld us); %u of %u rings taken, "
                "worst %ld.%ld ms, mean %ld us, %u over %d ms\n",
                pass ? "PASS" : "FAIL", woke_us, taken, rounds, worst / 1000, worst / 100 % 10,
                taken > 0 ? total / taken : 0, slow, SLOW_US / 1000);
    return pass ? (int)BELLTEST_OK : (int)STEP(9);
}
