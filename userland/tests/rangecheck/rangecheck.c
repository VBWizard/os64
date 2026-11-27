// rangecheck — what a syscall takes as a user buffer (MAPPING_LIST.md
// § The walk): the kernel checks every page of a range before it copies,
// asking the page table first and the list of mappings only for a page not
// there yet.
//
//   1. a buffer on this thread's stack, which no mapping covers, is taken,
//      and the page table answers for it (pages_by_table grows);
//   2. a fresh megabyte from map(), none of it touched, is taken for a read
//      into it, and its mapping answers for all of it in one lookup;
//   3. this thread's KERNEL stack, a supervisor page in the program's own
//      half of the address space, is refused both ways: read() into it and
//      write() out of it answer -2 and copy nothing;
//   4. a read() into this program's own text, present and read-only, is
//      refused;
//   5. a read() into the exit window, present, read-only and covered by no
//      mapping, is refused.
//
// Exit codes: 0x3C4E0000 success, 0x3C4E00NN the step that failed.

#include "os64/os64.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/str.h"

#define FAIL(step) return (int)(0x3C4E0000u | (step))
#define REFUSED (-2)
#define EXIT_WINDOW 0x6f110000ull   // TASK_EXIT_TRAMPOLINE_VIRT

static char s_text[16384];

static int64_t slurp(const char *path)
{
    int64_t h = os64_open(path, "r");
    if (h < 0)
        return -1;
    int64_t have = 0, n;
    while (have < (int64_t)sizeof(s_text) - 1 && (n = os64_read((int32_t)h, s_text + have, sizeof(s_text) - 1 - have)) > 0)
        have += n;
    os64_close((int32_t)h);
    s_text[have] = 0;
    return have;
}

// The first line of `text` that holds `word`, or NULL.
static const char *line_with(const char *text, const char *word)
{
    size_t k = os64_strlen(word);
    for (const char *line = text; line && *line;) {
        const char *end = os64_strchr(line, '\n');
        for (const char *c = line; *c && c != end; c++)
            if (os64_memcmp(c, word, k) == 0)
                return line;
        line = end ? end + 1 : NULL;
    }
    return NULL;
}

// pages_by_table's total, or -1.
static int64_t by_table(void)
{
    if (slurp("/proc/self/status") <= 0)
        return -1;
    const char *l = line_with(s_text, "pages_by_table\t");
    return l ? (int64_t)os64_atou(l + 15) : -1;
}

// A read of /proc/self/status into `dst`: what read() answered.
static int64_t read_into(void *dst, uint64_t len)
{
    int64_t h = os64_open("/proc/self/status", "r");
    if (h < 0)
        return -100;
    int64_t got = os64_read((int32_t)h, dst, len);
    os64_close((int32_t)h);
    return got;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // 1. A stack buffer: the page table's answer.
    char local[256];
    int64_t before = by_table();
    if (before < 0 || read_into(local, sizeof(local)) <= 0)
        FAIL(1);
    int64_t after = by_table();
    if (after <= before) {
        os64_printf("rangecheck: a read into the stack did not count pages_by_table (%ld, %ld)\n", (long)before,
                    (long)after);
        FAIL(1);
    }

    // 2. A fresh megabyte, untouched: the mapping answers for all of it.
    uint8_t *fresh = os64_map(1u << 20);
    if (fresh == NULL)
        FAIL(2);
    int64_t got = read_into(fresh + (1u << 20) - 8192, 8192);
    if (got <= 0) {
        os64_printf("rangecheck: a read into a fresh mapping answered %ld\n", (long)got);
        FAIL(2);
    }
    os64_unmap(fresh);

    // 3. This thread's kernel stack, from /proc/self/maps: its lowest page,
    //    the deepest and least likely to be in use, both ways.
    if (slurp("/proc/self/maps") <= 0)
        FAIL(3);
    const char *row = line_with(s_text, "[kstack:");
    uint64_t lo = 0, hi = 0;
    if (row == NULL || !os64_parse_range(row, &lo, &hi) || hi <= lo) {
        os64_printf("rangecheck: no [kstack] row in /proc/self/maps\n");
        FAIL(3);
    }
    got = read_into((void *)(uintptr_t)lo, 64);
    if (got != REFUSED) {
        os64_printf("rangecheck: read() into the kernel stack at %p answered %ld\n", (void *)(uintptr_t)lo, (long)got);
        FAIL(3);
    }
    int32_t ends[2];
    if (os64_pipe(ends) < 0)
        FAIL(3);
    got = os64_write(ends[1], (const void *)(uintptr_t)lo, 64);
    os64_close(ends[0]);
    os64_close(ends[1]);
    if (got != REFUSED) {
        os64_printf("rangecheck: write() out of the kernel stack at %p answered %ld\n", (void *)(uintptr_t)lo, (long)got);
        FAIL(3);
    }

    // 4. Its own text.
    got = read_into((void *)(uintptr_t)&main, 64);
    if (got != REFUSED) {
        os64_printf("rangecheck: read() into the program's text answered %ld\n", (long)got);
        FAIL(4);
    }

    // 5. The exit window: no mapping, present, read-only.
    got = read_into((void *)(uintptr_t)EXIT_WINDOW, 64);
    if (got != REFUSED) {
        os64_printf("rangecheck: read() into the exit window answered %ld\n", (long)got);
        FAIL(5);
    }
    return (int)0x3C4E0000u;
}
