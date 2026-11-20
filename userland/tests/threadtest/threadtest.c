// threadtest — proof that os64 can run more than one thing at once inside
// one program. The first ring-3 threads this OS has ever had (2026-08-02).
//
// Three threads, each handed a different argument, each returning a value
// derived from it, all joined and checked. That covers the whole contract:
// creation, argument passing, independent execution, return values, and
// the wait. It also proves the threads genuinely SHARE the address space —
// they all increment one global, and the total has to add up.
//
// And the start out of handles: with the program's handle table full, the
// kernel still starts the thread (detached) and answers an error, so
// os64_thread's fn runs although its start "failed", while
// os64_thread_or_nothing's fn never does (os64/thread.h).
//
// Exit codes: 0x1H2EAD00 success, and a distinct code per failed step.

#include "os64/os64.h"

#define THREADTEST_OK        0x1B2EAD00
#define THREADTEST_NO_START  0x1B2EAD01
#define THREADTEST_BAD_JOIN  0x1B2EAD02
#define THREADTEST_BAD_VALUE 0x1B2EAD03
#define THREADTEST_BAD_SHARE 0x1B2EAD04
#define THREADTEST_NO_FILL   0x1B2EAD05
#define THREADTEST_RAN       0x1B2EAD06

#define THREADS 3

// Shared, on purpose: threads share everything, and this is the cheapest
// possible demonstration. Each thread adds its own number exactly once, so
// the total is knowable — no lock needed because no two threads touch the
// same slot (the day they do, os64 will need locks, and that is the
// deliberate v1 debt).
static volatile int64_t shared_slots[THREADS];

static int64_t worker(void *arg)
{
    int64_t n = (int64_t)arg;

    // Burn a little so the threads genuinely overlap rather than each
    // finishing before the next is even created.
    for (volatile int64_t i = 0; i < 2000000; i++)
        ;

    shared_slots[n] = n + 1;      // our slot, ours alone
    return n * 100;               // the answer we'll be asked for
}

static volatile int s_ran_plain, s_ran_sure;

static int64_t mark_plain(void *arg) { (void)arg; s_ran_plain = 1; return 0; }
static int64_t mark_sure(void *arg) { (void)arg; s_ran_sure = 1; return 0; }

// Fills the handle table with opens of /dev/null, starts one thread each
// way, and lets the handles go. Returns an exit code, or 0.
static int out_of_handles(void)
{
    static int32_t held[512];
    int n = 0;
    while (n < 512) {
        int64_t h = os64_open("/dev/null", "r");
        if (h < 0)
            break;
        held[n++] = (int32_t)h;
    }
    int code = 0;
    if (n == 512) {
        os64_printf("threadtest: 512 handles and no refusal: the table is bigger than this test\n");
        code = THREADTEST_NO_FILL;
    } else {
        int64_t plain = os64_thread(mark_plain, 0);
        int64_t sure = os64_thread_or_nothing(mark_sure, 0);
        os64_sleep(300);
        if (plain >= 0 || sure >= 0) {
            os64_printf("threadtest: a start succeeded with the handle table full (%ld, %ld)\n", plain, sure);
            code = THREADTEST_NO_FILL;
        } else if (!s_ran_plain) {
            os64_printf("threadtest: os64_thread's refused start did not run (the hazard this proves is gone?)\n");
            code = THREADTEST_NO_FILL;
        } else if (s_ran_sure) {
            os64_printf("threadtest: os64_thread_or_nothing's fn ran after its start failed\n");
            code = THREADTEST_RAN;
        }
    }
    for (int i = 0; i < n; i++)
        os64_close(held[i]);
    return code;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int64_t handles[THREADS];
    for (int64_t i = 0; i < THREADS; i++)
    {
        handles[i] = os64_thread(worker, (void *)i);
        if (handles[i] < 0)
        {
            os64_printf("threadtest: could not start thread %ld\n", i);
            return THREADTEST_NO_START;
        }
    }

    int64_t total = 0;
    for (int64_t i = 0; i < THREADS; i++)
    {
        int64_t answer = -1;
        if (os64_thread_join((int32_t)handles[i], &answer) < 0)
        {
            os64_printf("threadtest: join of thread %ld failed\n", i);
            return THREADTEST_BAD_JOIN;
        }
        if (answer != i * 100)
        {
            os64_printf("threadtest: thread %ld returned %ld, expected %ld\n",
                        i, answer, i * 100);
            return THREADTEST_BAD_VALUE;
        }
        os64_close((int32_t)handles[i]);
        total += answer;
    }

    // The shared-memory half: every thread wrote its own slot in OUR
    // address space, and we can read all of them.
    int64_t sum = 0;
    for (int64_t i = 0; i < THREADS; i++)
        sum += shared_slots[i];
    if (sum != (THREADS * (THREADS + 1)) / 2)
    {
        os64_printf("threadtest: shared memory sum %ld, expected %d\n",
                    sum, (THREADS * (THREADS + 1)) / 2);
        return THREADTEST_BAD_SHARE;
    }

    // Silent on success, on purpose: this runs on every boot as a kernel
    // test, and a passing test that narrates itself is just noise on the
    // boot screen. The framework prints the verdict; the failure paths
    // above stay chatty, because a FAILING test should say exactly what it
    // saw before its exit code has to speak for it.
    (void)total;
    int code = out_of_handles();
    return code != 0 ? code : THREADTEST_OK;
}
