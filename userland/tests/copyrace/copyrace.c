// copyrace — a syscall's copy that outlives its check costs the program,
// not the machine (docs/design/pending/USER_COPY.md).
//
// The kernel checks a user buffer, then copies through it. A sibling thread
// can take the buffer away between the two, and the copy then faults in
// ring 0 on a page no longer there; the fault resumes at the copy's failure
// exit and the call answers BAD_USER_DATA (-2). Two doors:
//
//   1. a sibling's unmap: one thread reads a file into a fresh map region
//      and writes such a region into a pipe a third thread drains, while
//      a second unmaps the region at a moment of its choosing;
//   2. a thread's end: one thread reads into a buffer on a sibling's stack
//      while that sibling ends and the kernel reaps its stack.
//
// Passing: every call answered a byte count or -2, and the machine lives.
// It prints, per door, how many calls answered -2 and how many copies the
// kernel recovered (/proc/self/status's copy_recoveries, read before, between
// and after the doors). A -2 can also be the check refusing memory already
// gone, with no fault at all, so the recoveries are what say a door reached
// the net; a kernel without the recovery panics here.
//
//   copyrace [rounds]      (default 2000 for door 1, a quarter for door 2)

#include "os64/os64.h"

#define BADGE      0xC0B70000u
#define REGION     (256u * 1024u)
#define FILE_PATH  "/lib/libav.so"
#define STACK_BUF  (64u * 1024u)

static volatile int s_go, s_unmapped, s_stop;
static void *volatile s_region;
static int32_t s_file, s_pipe[2];
static long s_bad, s_ok, s_other;
static uint64_t s_seed = 0x2545F4914F6CDD1Dull;

static uint32_t rnd(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return (uint32_t)s_seed;
}

static void tally(int64_t r)
{
    if (r >= 0) s_ok++;
    else if (r == -2) s_bad++;
    else s_other++;
}

// Door 1's unmapper: waits for the copier to start, a few microseconds more,
// then takes the region away.
static int64_t unmapper(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        if (!__atomic_load_n(&s_go, __ATOMIC_ACQUIRE)) {
            os64_yield();
            continue;
        }
        int64_t until = os64_micros() + (int64_t)(rnd() % 400);
        while (os64_micros() < until)
            ;
        os64_unmap(s_region);
        __atomic_store_n(&s_go, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_unmapped, 1, __ATOMIC_RELEASE);
    }
    return 0;
}

// Door 1's drain, so a write into the pipe never waits long.
static int64_t drainer(void *arg)
{
    (void)arg;
    static char sink[64 * 1024];
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE))
        if (os64_read(s_pipe[0], sink, sizeof(sink)) <= 0)
            break;
    return 0;
}

static int door_unmap(int rounds)
{
    int64_t u = os64_thread_named(unmapper, NULL, "unmapper");
    int64_t d = os64_thread_named(drainer, NULL, "drainer");
    if (u < 0 || d < 0)
        return 1;
    for (int i = 0; i < rounds; i++) {
        void *region = os64_map(REGION);
        if (region == NULL)
            return 2;
        s_region = region;
        __atomic_store_n(&s_unmapped, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_go, 1, __ATOMIC_RELEASE);
        // The copy out (a file read into the region) or the copy in (the
        // region written into the pipe); the region is never touched here,
        // only handed to the kernel, so its going away cannot fault ring 3.
        if (i & 1) {
            os64_seek(s_file, (int64_t)(rnd() % 4) * REGION, OS64_SEEK_SET);
            tally(os64_read(s_file, region, REGION));
        } else {
            tally(os64_write(s_pipe[1], region, REGION));
        }
        while (!__atomic_load_n(&s_unmapped, __ATOMIC_ACQUIRE))
            os64_yield();
    }
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    os64_close(s_pipe[1]);
    os64_thread_join((int32_t)u, NULL);
    os64_thread_join((int32_t)d, NULL);
    os64_close((int32_t)u);
    os64_close((int32_t)d);
    return 0;
}

// Door 2: a thread that publishes a buffer on its own stack and ends. The
// address outlives the stack on purpose (it is what the door is), so it is
// published as a number.
static volatile uintptr_t s_stack_buf;
static int64_t short_lived(void *arg)
{
    (void)arg;
    char buf[STACK_BUF];
    buf[0] = 1;
    __atomic_store_n(&s_stack_buf, (uintptr_t)buf, __ATOMIC_RELEASE);
    return buf[0];
}

static int door_stack(int rounds)
{
    for (int i = 0; i < rounds; i++) {
        __atomic_store_n(&s_stack_buf, 0, __ATOMIC_RELEASE);
        int64_t t = os64_thread_named(short_lived, NULL, "short-lived");
        if (t < 0)
            return 1;
        while (__atomic_load_n(&s_stack_buf, __ATOMIC_ACQUIRE) == 0)
            os64_yield();
        char *buf = (char *)__atomic_load_n(&s_stack_buf, __ATOMIC_ACQUIRE);
        os64_close((int32_t)t);
        // Read into it until the kernel refuses: its stack is reaped some
        // while after it ends, and a read may be under way when it is.
        for (int tries = 0; tries < 200; tries++) {
            os64_seek(s_file, 0, OS64_SEEK_SET);
            int64_t r = os64_read(s_file, buf, STACK_BUF);
            tally(r);
            if (r < 0)
                break;
        }
    }
    return 0;
}

static uint64_t recoveries(void)
{
    static char text[4096];
    int64_t h = os64_open("/proc/self/status", "r");
    if (h < 0)
        return 0;
    int64_t n = os64_read((int32_t)h, text, sizeof(text) - 1);
    os64_close((int32_t)h);
    if (n <= 0)
        return 0;
    text[n] = 0;
    static const char key[] = "copy_recoveries\t";
    for (const char *at = text; *at; at++)
        if (os64_memcmp(at, key, sizeof(key) - 1) == 0)
            return os64_atou(at + sizeof(key) - 1);
    return 0;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? (int)os64_atou(argv[1]) : 2000;
    s_seed ^= os64_micros();
    int64_t f = os64_open(FILE_PATH, "r");
    if (f < 0 || os64_pipe(s_pipe) < 0) {
        os64_printf("copyrace: cannot open %s or a pipe\n", FILE_PATH);
        return (int)(BADGE | 1);
    }
    s_file = (int32_t)f;
    uint64_t rec0 = recoveries();
    if (door_unmap(rounds) != 0)
        return (int)(BADGE | 2);
    long bad1 = s_bad, ok1 = s_ok;
    uint64_t rec1 = recoveries();
    if (door_stack(rounds / 4) != 0)
        return (int)(BADGE | 3);
    uint64_t rec2 = recoveries();
    os64_printf("copyrace: door 1 (unmap): %ld calls answered a count, %ld refused, %lu copies recovered\n",
                ok1, bad1, (unsigned long)(rec1 - rec0));
    os64_printf("copyrace: door 2 (a reaped stack): %ld answered a count, %ld refused, %lu copies recovered\n",
                s_ok - ok1, s_bad - bad1, (unsigned long)(rec2 - rec1));
    os64_printf("copyrace: %ld other answers\n", s_other);
    if (s_other != 0) {
        os64_printf("copyrace: FAIL - a call answered something other than a count or -2\n");
        return (int)(BADGE | 4);
    }
    return (int)BADGE;
}
