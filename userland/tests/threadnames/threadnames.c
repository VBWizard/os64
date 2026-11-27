// threadnames — a thread is named when it is made, and its status says so
// (docs/design/pending/THREAD_NAMES.md; abi/include/os64/thread_name.h).
//
// Steps, each a badge on failure:
//   1. this program's own first thread reads empty, and kworker's thread
//      reads "kworker" (a kernel task's thread is named for its task);
//   2. threads made named, unnamed, empty-named, with the longest name
//      (63 bytes), with a name crossing a page boundary, and with a name
//      whose buffer is overwritten the moment the call returns: each reads
//      back as given, the overwritten one as it was when made;
//   3. a name is refused, and no thread made, when it lies in memory that
//      was let go (-2), crosses into the guard page after a mapping (-2),
//      has no end within 64 bytes (-1), or holds a tab, DEL or a byte over
//      0x7E (-1);
//   4. the threads of step 2 are let go and each answers its join.
//
// Each status file is read directly, by its `name` line, so the fixture
// proves the kernel's half without leaning on a reader built over it.

#include <stdint.h>

#include "os64/os64.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/procfs.h"
#include "os64/str.h"
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"
#include "os64/thread.h"
#include "os64/thread_name.h"

// libos64's exit stub (thread_trampoline.S): where a thread function returns.
extern void os64_thread_trampoline(void);

#define FAIL(step) return (int)(0x7A3E0000u | (step))
#define PAGE 4096u

static char s_text[4096];

static int64_t read_text(const char *path)
{
    int64_t h = os64_open(path, "r");
    if (h < 0)
        return -1;
    int64_t have = 0, n;
    while (have < (int64_t)sizeof(s_text) - 1 &&
           (n = os64_read((int32_t)h, s_text + have, sizeof(s_text) - 1 - have)) > 0)
        have += n;
    os64_close((int32_t)h);
    s_text[have] = 0;
    return have;
}

// The value of `key`'s line (key, a tab, the value) copied into out; false
// when the file or the line is missing.
static bool status_value(const char *path, const char *key, char *out, size_t cap)
{
    if (read_text(path) < 0)
        return false;
    size_t k = os64_strlen(key);
    for (const char *line = s_text; *line;) {
        if (os64_memcmp(line, key, k) == 0 && line[k] == '\t') {
            const char *v = line + k + 1;
            size_t n = 0;
            while (v[n] && v[n] != '\n' && n + 1 < cap) {
                out[n] = v[n];
                n++;
            }
            out[n] = 0;
            return true;
        }
        const char *nl = os64_strchr(line, '\n');
        if (nl == NULL)
            break;
        line = nl + 1;
    }
    return false;
}

static bool thread_name_of(uint64_t task, uint64_t thread, char *out, size_t cap)
{
    char path[96];
    os64_snprintf(path, sizeof(path), "/proc/%lu/thread/%lu/status", task, thread);
    return status_value(path, "name", out, cap);
}

// "threads\t<made> made, ...": the made count, or -1.
static int64_t threads_made(void)
{
    char v[64];
    if (!status_value("/proc/self/status", "threads", v, sizeof(v)))
        return -1;
    return (int64_t)os64_atou(v);
}

static volatile int s_go;

static int64_t parked(void *arg)
{
    while (!__atomic_load_n(&s_go, __ATOMIC_ACQUIRE))
        os64_yield();
    return (int64_t)(uintptr_t)arg;
}

// The thread call with the name at a raw address, so a name the library's
// own checks never see reaches the kernel as it is.
static int64_t make_raw(const void *name, uint64_t arg)
{
    return (int64_t)os64_syscall4(SYSCALL_THREAD, (uint64_t)(uintptr_t)&parked, arg,
                                  (uint64_t)(uintptr_t)&os64_thread_trampoline,
                                  (uint64_t)(uintptr_t)name);
}

static int step_own_and_kernel(void)
{
    uint64_t self = os64_taskid();
    char name[OS64_THREAD_NAME_MAX];
    // A task's first thread carries the task's ID.
    if (!thread_name_of(self, self, name, sizeof(name)) || name[0] != 0) {
        os64_printf("threadnames: my first thread should read unnamed, read \"%s\"\n", name);
        return 1;
    }

    static os64_proc_info_t tasks[256];
    int32_t n = os64_proc_snapshot(tasks, 256);
    for (int32_t i = 0; i < n; i++) {
        if (!os64_streq(tasks[i].name, "kworker"))
            continue;
        if (!thread_name_of(tasks[i].pid, tasks[i].pid, name, sizeof(name)) ||
            !os64_streq(name, "kworker")) {
            os64_printf("threadnames: kworker's thread should read \"kworker\", read \"%s\"\n", name);
            return 1;
        }
        return 0;
    }
    os64_printf("threadnames: no kworker task in /proc\n");
    return 1;
}

#define MADE 6
static const char *const s_expect[MADE] = {
    "decode", "", "", "", "page-cross", "before",
};
static char s_longest[OS64_THREAD_NAME_MAX];
static int64_t s_handles[MADE];

static int step_made(void)
{
    // The longest name the rule takes: 63 bytes, every printable edge in it.
    for (int i = 0; i < OS64_THREAD_NAME_MAX - 1; i++)
        s_longest[i] = (char)(0x20 + (i * 7) % 95);
    s_longest[0] = ' ';
    s_longest[OS64_THREAD_NAME_MAX - 2] = '~';
    s_longest[OS64_THREAD_NAME_MAX - 1] = 0;

    // A name that starts four bytes before a page ends.
    char *two = os64_map(2 * PAGE);
    if (two == NULL)
        return 1;
    char *cross = two + PAGE - 4;
    os64_strcopy(cross, 32, "page-cross");

    char scratch[16];
    os64_strcopy(scratch, sizeof(scratch), "before");

    s_handles[0] = os64_thread_named(parked, (void *)1, "decode");
    s_handles[1] = os64_thread(parked, (void *)2);
    s_handles[2] = os64_thread_named(parked, (void *)3, "");
    s_handles[3] = os64_thread_named(parked, (void *)4, s_longest);
    s_handles[4] = os64_thread_named(parked, (void *)5, cross);
    s_handles[5] = os64_thread_named(parked, (void *)6, scratch);
    os64_strcopy(scratch, sizeof(scratch), "after!");   // the kernel kept its own copy
    for (int i = 0; i < MADE; i++)
        if (s_handles[i] < 0) {
            os64_printf("threadnames: thread %d was refused (%ld)\n", i, (long)s_handles[i]);
            return 1;
        }

    // Every thread's name, matched against what was given: each named one
    // exactly once, and the unnamed ones (plus this first thread) empty.
    static os64_thread_info_t threads[64];
    int32_t n = os64_proc_threads(os64_taskid(), threads, 64);
    if (n != MADE + 1) {
        os64_printf("threadnames: /proc lists %d threads, %d expected\n", n, MADE + 1);
        return 1;
    }
    int seen[MADE] = { 0 }, empty = 0, longest = 0;
    for (int32_t t = 0; t < n; t++) {
        char name[OS64_THREAD_NAME_MAX + 8];
        if (!thread_name_of(os64_taskid(), threads[t].tid, name, sizeof(name))) {
            os64_printf("threadnames: thread %lu's status has no name line\n", (unsigned long)threads[t].tid);
            return 1;
        }
        if (name[0] == 0) {
            empty++;
            continue;
        }
        if (os64_streq(name, s_longest)) {
            longest++;
            continue;
        }
        bool known = false;
        for (int i = 0; i < MADE; i++)
            if (s_expect[i][0] && os64_streq(name, s_expect[i])) {
                seen[i]++;
                known = true;
            }
        if (!known) {
            os64_printf("threadnames: a thread reads \"%s\", which no call gave\n", name);
            return 1;
        }
    }
    if (empty != 3 || longest != 1 || seen[0] != 1 || seen[4] != 1 || seen[5] != 1) {
        os64_printf("threadnames: names read back wrong (empty %d, longest %d, decode %d, page-cross %d, before %d)\n",
                    empty, longest, seen[0], seen[4], seen[5]);
        return 1;
    }
    return 0;
}

static int refused(const char *what, const void *name, int64_t want)
{
    int64_t before = threads_made();
    int64_t r = make_raw(name, 99);
    int64_t after = threads_made();
    if (r != want || before < 0 || after != before) {
        os64_printf("threadnames: a name %s answered %ld (want %ld); threads made %ld before, %ld after\n",
                    what, (long)r, (long)want, (long)before, (long)after);
        return 1;
    }
    return 0;
}

static int step_refused(void)
{
    // Memory that was let go.
    char *gone = os64_map(PAGE);
    if (gone == NULL)
        return 1;
    os64_strcopy(gone, PAGE, "gone");
    os64_unmap(gone);
    if (refused("in memory let go", gone, -2))
        return 1;

    // Ten bytes at a mapping's end, running into its guard page.
    char *edge = os64_map(PAGE);
    if (edge == NULL)
        return 1;
    os64_memset(edge + PAGE - 10, 'g', 10);
    if (refused("crossing into the guard page", edge + PAGE - 10, -2))
        return 1;

    // 64 bytes with no end among them (the 65th is the end).
    char *roomy = os64_map(PAGE);
    if (roomy == NULL)
        return 1;
    os64_memset(roomy, 'r', OS64_THREAD_NAME_MAX);
    roomy[OS64_THREAD_NAME_MAX] = 0;
    if (refused("of 64 bytes", roomy, -1))
        return 1;

    if (refused("with a tab", "a\tb", -1) ||
        refused("with DEL", "a\x7f", -1) ||
        refused("with a byte over 0x7E", "caf\xe9", -1))
        return 1;
    return 0;
}

static int step_joined(void)
{
    __atomic_store_n(&s_go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < MADE; i++) {
        int64_t v = -1;
        if (os64_thread_join((int32_t)s_handles[i], &v) < 0 || v != i + 1) {
            os64_printf("threadnames: thread %d answered %ld\n", i, (long)v);
            return 1;
        }
        os64_close((int32_t)s_handles[i]);
    }
    return 0;
}

int main(void)
{
    if (step_own_and_kernel())
        FAIL(1);
    if (step_made())
        FAIL(2);
    if (step_refused())
        FAIL(3);
    if (step_joined())
        FAIL(4);
    os64_printf("threadnames: kworker named, my first thread unnamed; 6 threads read back as given; 7 bad names refused with no thread made\n");
    return 0x7A3E0000;
}
