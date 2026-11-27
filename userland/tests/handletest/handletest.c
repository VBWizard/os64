// handletest — a task's handle table grows past its first block and keeps
// every number it has handed out (kernel/include/handle_blocks.h;
// docs/design/pending/VIDEO_PLANE.md § 7). Run by testrun.
//
// What it proves, each step a code of its own:
//   1. /proc/self/handles begins with the table's line: held, room 64,
//      ceiling 4096, most, failed;
//   2. 250 pipes and 500 bells take 1000 handles more, every number new,
//      and the room reported covers what is held;
//   3. every pipe carries its own bytes and every bell its own ring, so a
//      number handed out early still names what it named;
//   4. closed in a mixed order and opened again, the new numbers are the
//      lowest freed, and the handles still open still carry their bytes;
//   5. two threads: one opens bells until the table has grown past 1024
//      while the other writes and reads through pipes it already holds;
//   6. at the ceiling an open is refused and counted, and a thread is
//      refused with no thread made (its handle is asked for first);
//   7. everything closed, the table holds what it held at the start.
//
// Exit codes: 0x4A7D0000 success, 0x4A7D00NN the step that failed.

#include "os64/os64.h"
#include "os64/bell.h"
#include "os64/thread.h"

#define PIPES 250
#define BELLS 500
#define CEILING 4096
static void said(int step);
#define FAIL(step) (said(step), os64_exit(0x4A7D0000u | (step)))

static int32_t s_pipes[PIPES][2];
static os64_bell_t s_bells[BELLS];
static os64_bell_t s_fill[CEILING];

typedef struct {
    uint64_t held, room, ceiling, most, failed;
} table_t;

static char s_line[512];   // the table line last read, for a failure to show

// The first line of /proc/self/handles, read into `out`.
static bool table_read(table_t *out)
{
    static char text[8192];
    int64_t h = os64_open("/proc/self/handles", "r");
    if (h < 0)
        return false;
    size_t have = 0;
    for (;;) {
        int64_t n = os64_read((int32_t)h, text + have, 64);
        if (n <= 0 || have + (size_t)n >= 512)
            break;
        have += (size_t)n;
    }
    os64_close((int32_t)h);
    text[have] = '\0';
    size_t n = 0;
    while (text[n] != '\0' && text[n] != '\n' && n + 1 < sizeof(s_line)) {
        s_line[n] = text[n];
        n++;
    }
    s_line[n] = '\0';
    const char *word[] = {"table\theld ", "\troom ", "\tceiling ", "\tmost ", "\tfailed "};
    uint64_t *field[] = {&out->held, &out->room, &out->ceiling, &out->most, &out->failed};
    const char *at = text;
    for (int i = 0; i < 5; i++) {
        const char *w = word[i];
        while (*w != '\0' && *at == *w) {
            at++;
            w++;
        }
        if (*w != '\0')
            return false;
        uint64_t v = 0;
        if (*at < '0' || *at > '9')
            return false;
        while (*at >= '0' && *at <= '9')
            v = v * 10 + (uint64_t)(*at++ - '0');
        *field[i] = v;
    }
    return *at == '\n';
}

// The task's threads, from /proc/self/status.
static int64_t threads_now(void)
{
    static char text[4096];
    int64_t h = os64_open("/proc/self/status", "r");
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
    for (size_t i = 0; i + 8 < have; i++)
        if ((i == 0 || text[i - 1] == '\n') && os64_memcmp(text + i, "threads\t", 8) == 0)
            return os64_atoi(text + i + 8);
    return -1;
}

// A failing step says what it last read before its exit code speaks.
static void said(int step)
{
    os64_printf("handletest: step %d failed; the table last read: %s\n", step, s_line);
}

// Pipe p carries its own byte: write it, read it back.
static bool pipe_carries(int p)
{
    uint8_t out = (uint8_t)(p * 7 + 1), in = 0;
    return os64_write(s_pipes[p][1], &out, 1) == 1 && os64_read(s_pipes[p][0], &in, 1) == 1 && in == out;
}

// Bell b holds its own ring: rung, it answers; another bell does not.
static bool bell_rings(int b)
{
    if (os64_bell_ring(&s_bells[b]) != 0 || os64_bell_wait(&s_bells[b], 0) != 1)
        return false;
    return os64_bell_wait(&s_bells[(b + 1) % BELLS], 0) == OS64_ERR_TIMEOUT;
}

static volatile int s_reader_bad;
static volatile int s_grow_done;

// The second thread of step 5: round trips through sixteen pipes still open
// (step 4 closed the odd ones) until the opener has grown the table,
// counting any that come back wrong.
static int64_t reader(void *arg)
{
    (void)arg;
    uint64_t rounds = 0;
    while (!__atomic_load_n(&s_grow_done, __ATOMIC_ACQUIRE) || rounds < 64) {
        for (int p = 0; p < 32; p += 2)
            if (!pipe_carries(p))
                __atomic_add_fetch(&s_reader_bad, 1, __ATOMIC_RELAXED);
        rounds++;
    }
    return (int64_t)rounds;
}

// The thread step 6 asks for at the ceiling. It must never be made; if it
// is, it stays alive to be counted, and the task's exit ends it.
static int64_t never_runs(void *arg)
{
    (void)arg;
    for (;;)
        os64_sleep(1000);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    table_t start, t;
    // 1. The table's line. /proc takes its snapshot before the open's own
    // handle exists, so `held` is what the program holds.
    if (!table_read(&start) || start.room != 64 || start.ceiling != CEILING || start.held < 3 ||
        start.most < start.held)
        FAIL(1);

    // 2. A thousand handles more.
    int32_t highest = 2;
    for (int p = 0; p < PIPES; p++) {
        if (os64_pipe(s_pipes[p]) < 0 || s_pipes[p][0] <= highest || s_pipes[p][1] <= s_pipes[p][0])
            FAIL(2);
        highest = s_pipes[p][1];
    }
    for (int b = 0; b < BELLS; b++) {
        if (os64_bell_make(&s_bells[b]) < 0 || s_bells[b].handle <= highest)
            FAIL(2);
        highest = s_bells[b].handle;
    }
    if (!table_read(&t) || t.held != start.held + 2 * PIPES + BELLS || t.room < t.held || t.room > 2048 ||
        t.most < t.held || t.failed != start.failed)
        FAIL(2);

    // 3. Every number still names what it named.
    for (int p = 0; p < PIPES; p++)
        if (!pipe_carries(p))
            FAIL(3);
    for (int b = 0; b < BELLS; b++)
        if (!bell_rings(b))
            FAIL(3);

    // 4. Close a mixed set (every third bell, the odd pipes' read ends'
    // pipes whole), open as many bells again, and look.
    int32_t freed_low = 0x7fffffff;
    for (int b = 0; b < BELLS; b += 3) {
        if (s_bells[b].handle < freed_low)
            freed_low = s_bells[b].handle;
        os64_bell_close(&s_bells[b]);
    }
    for (int p = 1; p < PIPES; p += 2) {
        if (s_pipes[p][0] < freed_low)
            freed_low = s_pipes[p][0];
        os64_close(s_pipes[p][0]);
        os64_close(s_pipes[p][1]);
        s_pipes[p][0] = s_pipes[p][1] = -1;
    }
    os64_bell_t again;
    if (os64_bell_make(&again) < 0 || again.handle != freed_low)
        FAIL(4);
    os64_bell_close(&again);
    for (int b = 0; b < BELLS; b += 3)
        if (os64_bell_make(&s_bells[b]) < 0)
            FAIL(4);
    for (int p = 0; p < PIPES; p += 2)
        if (!pipe_carries(p))
            FAIL(4);
    for (int b = 0; b < BELLS; b++)
        if (!bell_rings(b))
            FAIL(4);

    // 5. Grow while another thread works through handles it holds.
    int64_t th = os64_thread(reader, NULL);
    if (th < 0)
        FAIL(5);
    int filled = 0;
    while (filled < CEILING) {
        if (!table_read(&t))
            FAIL(5);
        if (t.room > 1024)
            break;
        if (os64_bell_make(&s_fill[filled]) < 0)
            FAIL(5);
        filled++;
    }
    __atomic_store_n(&s_grow_done, 1, __ATOMIC_RELEASE);
    int64_t rounds = 0;
    if (os64_thread_join((int32_t)th, &rounds) < 0 || rounds < 64 || s_reader_bad != 0)
        FAIL(5);
    os64_close((int32_t)th);

    // 6. The ceiling: an open is refused and counted, and so is a thread,
    // with none made. Reading /proc takes a handle of its own, so the
    // threads are counted before the table is full and the table is read
    // after one bell has made room for the read.
    int64_t threads = threads_now();
    if (threads < 1)
        FAIL(6);
    for (;;) {
        if (filled == CEILING)
            FAIL(6);
        if (os64_bell_make(&s_fill[filled]) < 0)
            break;
        filled++;
    }
    if (os64_thread(never_runs, NULL) >= 0)
        FAIL(6);
    os64_bell_close(&s_fill[--filled]);
    if (threads_now() != threads)
        FAIL(6);
    // One entry free: the bell given back, which the read's own handle
    // takes after the snapshot.
    if (!table_read(&t) || t.held != CEILING - 1 || t.room != CEILING || t.failed != start.failed + 2)
        FAIL(6);

    // 7. Everything closed.
    for (int i = 0; i < filled; i++)
        os64_bell_close(&s_fill[i]);
    for (int b = 0; b < BELLS; b++)
        os64_bell_close(&s_bells[b]);
    for (int p = 0; p < PIPES; p += 2) {
        os64_close(s_pipes[p][0]);
        os64_close(s_pipes[p][1]);
    }
    if (!table_read(&t) || t.held != start.held || t.room != CEILING || t.most != CEILING)
        FAIL(7);
    return (int)0x4A7D0000u;
}
