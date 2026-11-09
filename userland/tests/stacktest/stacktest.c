// stacktest — a program's main thread gets the stack it declares
// (SCRIPT_STACK.md).
//
// This program is linked with `-z stack-size=4194304` (GNUmakefile's
// BIG_STACK_PROGRAMS), which writes 4 MiB into its PT_GNU_STACK header; the
// kernel reads it there (elf_can_load) and gives the main thread that stack.
// Three things must hold, and the pass code says all three did:
//
//   1. the main thread's /proc/self/maps row, [stack:<tid>], is 4 MiB: the
//      size the kernel records (esp3Size) is the one it reports;
//   2. the stack is really that deep: a recursion that touches 3.3 MB of it
//      returns, where on the default 1 MiB it would die in the guard below
//      (exit 139, stackprobe's death);
//   3. a thread this program spawns gets the default 1 MiB: only the main
//      thread takes the declared size.
//
// 0x57AC600D is "STACK GOOD"; each failure exits its own 0x0BADn and says
// which on stderr, so testrun's line names it.

#include "os64/os64.h"
#include "os64/thread.h"

#define DECLARED   ((uint64_t)4 * 1024 * 1024)
#define DEFAULT    ((uint64_t)1 * 1024 * 1024)
#define FRAME      4096
#define DEPTH      800              // 800 frames of 4 KiB: 3.3 MB, past 1 MiB, inside 4 MiB

static bool hex_word(const char **text, uintptr_t *value)
{
    const char *p = *text;
    unsigned digits = 0;
    *value = 0;
    for (;; p++) {
        unsigned c = (unsigned char)*p, n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else break;
        if (++digits > 16)
            return false;
        *value = *value * 16 + n;
    }
    *text = p;
    return digits != 0;
}

// The size of the calling thread's stack, from its /proc/self/maps row: the
// [stack:<tid>] one whose bounds hold a local. 0 when there is none. One
// caller at a time (the main thread reads, then waits on the thread that
// reads), so one static buffer serves both.
static uint64_t own_stack(void)
{
    static char maps[32768];
    int64_t handle = os64_open("/proc/self/maps", "r");
    if (handle < 0)
        return 0;
    size_t length = 0;
    while (length < sizeof(maps) - 1) {
        int64_t got = os64_read((int32_t)handle, maps + length, sizeof(maps) - 1 - length);
        if (got <= 0)
            break;
        length += (size_t)got;
    }
    os64_close((int32_t)handle);
    maps[length] = '\0';
    uintptr_t here = (uintptr_t)&handle;
    for (const char *line = maps; *line != '\0';) {
        const char *end = os64_strchr(line, '\n');
        const char *p = line;
        uintptr_t low, high;
        if (hex_word(&p, &low) && *p++ == '-' && hex_word(&p, &high) && here >= low && here < high) {
            const char *label = os64_strchr(p, '[');
            if (label != NULL && (end == NULL || label < end) && os64_memcmp(label, "[stack:", 7) == 0)
                return high - low;
            return 0;
        }
        if (end == NULL)
            break;
        line = end + 1;
    }
    return 0;
}

// Each frame holds a page and writes every byte of it, so the depth is real
// stack, not one the compiler folded away.
static uint64_t deep(int n)
{
    volatile uint8_t page[FRAME];
    for (int i = 0; i < FRAME; i++)
        page[i] = (uint8_t)(n + i);
    uint64_t below = n > 0 ? deep(n - 1) : 0;
    return below + page[n % FRAME];
}

static int64_t spawned(void *arg)
{
    (void)arg;
    return (int64_t)own_stack();
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    uint64_t mine = own_stack();
    if (mine != DECLARED) {
        os64_hprintf(OS64_STDERR, "stacktest: FAIL: the main thread's stack is %lu bytes, declared %lu\n",
                     (unsigned long)mine, (unsigned long)DECLARED);
        return 0x0BAD1;
    }

    (void)deep(DEPTH);       // on a 1 MiB stack this dies in the guard: exit 139

    int64_t handle = os64_thread(spawned, NULL);
    int64_t theirs = 0;
    if (handle < 0 || os64_thread_join((int32_t)handle, &theirs) < 0) {
        os64_hprintf(OS64_STDERR, "stacktest: FAIL: could not run a thread (%ld)\n", (long)handle);
        return 0x0BAD2;
    }
    if ((uint64_t)theirs != DEFAULT) {
        os64_hprintf(OS64_STDERR, "stacktest: FAIL: a spawned thread's stack is %ld bytes, the default is %lu\n",
                     (long)theirs, (unsigned long)DEFAULT);
        return 0x0BAD3;
    }

    os64_puts("stacktest: the main thread has its declared 4 MiB, used to 3.3 MB; a thread has 1 MiB\n");
    return 0x57AC600D;
}
